// Copyright 2013 Velodyne Acoustics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkVelodyneHDLReader.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

  This software is distributed WITHOUT ANY WARRANTY; without even
  the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "vtkVelodyneHDLReader.h"
#include "vtkLidarReaderInternal.h"

#include "vtkPacketFileReader.h"
#include "vtkPacketFileWriter.h"
#include "vtkRollingDataAccumulator.h"
#include "vtkVelodyneTransformInterpolator.h"

#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkDataArray.h>
#include <vtkDoubleArray.h>
#include <vtkFloatArray.h>
#include <vtkInformation.h>
#include <vtkInformationVector.h>
#include <vtkMath.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkStreamingDemandDrivenPipeline.h>
#include <vtkUnsignedCharArray.h>
#include <vtkUnsignedIntArray.h>
#include <vtkUnsignedShortArray.h>

#include <vtkTransform.h>

#include <algorithm>
#include <cmath>
#include <sstream>

#include <boost/filesystem.hpp>
#include <boost/foreach.hpp>
#include <boost/preprocessor.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include <Eigen/Dense>

#ifdef _MSC_VER
#include <boost/cstdint.hpp>
typedef boost::uint8_t uint8_t;
#else
#include <stdint.h>
#endif

#include "vtkDataPacket.h"
using namespace DataPacketFixedLength;

namespace
{

// Structure to compute RPM and handle degenerated cases
struct RPMCalculator
{
  // Determines if the rpm computation is available
  bool IsReady;
  // Determines if the corresponding value has been set
  bool ValueReady[4];
  int MinAngle;
  int MaxAngle;
  unsigned int MinTime;
  unsigned int MaxTime;

  void Reset()
  {
    this->IsReady = false;
    this->ValueReady[0] = false;
    this->ValueReady[1] = false;
    this->ValueReady[2] = false;
    this->ValueReady[3] = false;
    this->MinAngle = std::numeric_limits<int>::max();
    this->MaxAngle = std::numeric_limits<int>::min();
    this->MinTime = std::numeric_limits<unsigned int>::max();
    this->MaxTime = std::numeric_limits<unsigned int>::min();
  }

  double GetRPM()
  {
    // If the calculator is not ready i.e : one
    // of the attributes is not initialized yet
    // (MaxAngle, MinAngle, MaxTime, MinTime)
    if (!this->IsReady)
    {
      return 0;
    }

    // delta angle in number of laps
    double dAngle = static_cast<double>(this->MaxAngle - this->MinAngle) / (100.0 * 360.0);

    // delta time in minutes
    double dTime = static_cast<double>(this->MaxTime - this->MinTime) / (60e6);

    // epsilon to test if the delta time / angle is not too small
    const double epsilon = 1e-12;

    // if one the deltas is too small
    if ((std::abs(dTime) < epsilon) || (std::abs(dAngle) < epsilon))
    {
      return 0;
    }

    return dAngle / dTime;
  }

  void AddData(HDLDataPacket* HDLPacket, unsigned int rawtime)
  {
    if (HDLPacket->firingData[0].rotationalPosition < this->MinAngle)
    {
      this->MinAngle = HDLPacket->firingData[0].rotationalPosition;
      this->ValueReady[0] = true;
    }
    if (HDLPacket->firingData[0].rotationalPosition > this->MaxAngle)
    {
      this->MaxAngle = HDLPacket->firingData[0].rotationalPosition;
      this->ValueReady[1] = true;
    }
    if (rawtime < this->MinTime)
    {
      this->MinTime = rawtime;
      this->ValueReady[2] = true;
    }
    if (rawtime > this->MaxTime)
    {
      this->MaxTime = rawtime;
      this->ValueReady[3] = true;
    }

    // Check if all of the 4th parameters
    // have been set
    this->IsReady = true;
    for (int k = 0; k < 4; ++k)
    {
      this->IsReady &= this->ValueReady[k];
    }
  }
};

//-----------------------------------------------------------------------------
int MapFlags(unsigned int flags, unsigned int low, unsigned int high)
{
  return (flags & low ? -1 : flags & high ? 1 : 0);
}

//-----------------------------------------------------------------------------
int MapDistanceFlag(unsigned int flags)
{
  return MapFlags(flags & vtkVelodyneHDLReader::DUAL_DISTANCE_MASK,
    vtkVelodyneHDLReader::DUAL_DISTANCE_NEAR, vtkVelodyneHDLReader::DUAL_DISTANCE_FAR);
}

//-----------------------------------------------------------------------------
int MapIntensityFlag(unsigned int flags)
{
  return MapFlags(flags & vtkVelodyneHDLReader::DUAL_INTENSITY_MASK,
    vtkVelodyneHDLReader::DUAL_INTENSITY_LOW, vtkVelodyneHDLReader::DUAL_INTENSITY_HIGH);
}

//-----------------------------------------------------------------------------
double HDL32AdjustTimeStamp(int firingblock, int dsr, const bool isDualReturnMode)
{
  if (!isDualReturnMode)
  {
    return (firingblock * 46.08) + (dsr * 1.152);
  }
  else
  {
    return (firingblock / 2 * 46.08) + (dsr * 1.152);
  }
}

//-----------------------------------------------------------------------------
double VLP16AdjustTimeStamp(
  int firingblock, int dsr, int firingwithinblock, const bool isDualReturnMode)
{
  if (!isDualReturnMode)
  {
    return (firingblock * 110.592) + (dsr * 2.304) + (firingwithinblock * 55.296);
  }
  else
  {
    return (firingblock / 2 * 110.592) + (dsr * 2.304) + (firingwithinblock * 55.296);
  }
}

//-----------------------------------------------------------------------------
double VLP32AdjustTimeStamp(int firingblock, int dsr, const bool isDualReturnMode)
{
  if (!isDualReturnMode)
  {
    return (firingblock * 55.296) + (dsr / 2) * 2.304;
  }
  else
  {
    return (firingblock / 2 * 55.296) + (dsr / 2) * 2.304;
  }
}

//-----------------------------------------------------------------------------
double HDL64EAdjustTimeStamp(int firingblock, int dsr, const bool isDualReturnMode)
{
  const int dsrReversed = HDL_LASER_PER_FIRING - dsr - 1;
  const int firingblockReversed = HDL_FIRING_PER_PKT - firingblock - 1;
  if (!isDualReturnMode)
  {
    const double TimeOffsetMicroSec[4] = { 2.34, 3.54, 4.74, 6.0 };
    return (std::floor(static_cast<double>(firingblockReversed) / 2.0) * 48.0) +
      TimeOffsetMicroSec[(dsrReversed % 4)] + (dsrReversed / 4) * TimeOffsetMicroSec[3];
  }
  else
  {
    const double TimeOffsetMicroSec[4] = { 3.5, 4.7, 5.9, 7.2 };
    return (std::floor(static_cast<double>(firingblockReversed) / 4.0) * 57.6) +
      TimeOffsetMicroSec[(dsrReversed % 4)] + (dsrReversed / 4) * TimeOffsetMicroSec[3];
  }
}
} // End namespace

//-----------------------------------------------------------------------------
class FramingState
{
  int LastAzimuth;
  int LastAzimuthSlope;

public:
  FramingState() { reset(); }
  void reset()
  {
    LastAzimuth = -1;
    LastAzimuthSlope = 0;
  }
  bool hasChangedWithValue(const HDLFiringData& firingData)
  {
    bool hasLastAzimuth = (LastAzimuth != -1);
    bool azimuthFrameSplit = hasChangedWithValue(
      firingData.rotationalPosition, hasLastAzimuth, LastAzimuth, LastAzimuthSlope);
    return azimuthFrameSplit;
  }

  static bool hasChangedWithValue(int curValue, bool& hasLastValue, int& lastValue, int& lastSlope)
  {
    // If we dont have previous value, dont change
    if (!hasLastValue)
    {
      lastValue = curValue;
      hasLastValue = true;
      return false;
    }
    int curSlope = curValue - lastValue;
    lastValue = curValue;
    if (curSlope == 0)
      return false;
    int isSlopeSameDirection = curSlope * lastSlope;
    // curSlope has same sign as lastSlope: no change
    if (isSlopeSameDirection > 0)
      return false;
    // curSlope has different sign as lastSlope: change!
    else if (isSlopeSameDirection < 0)
    {
      lastSlope = 0;
      return true;
    }
    // LastAzimuthSlope not set: set the slope
    if (lastSlope == 0 && curSlope != 0)
    {
      lastSlope = curSlope;
      return false;
    }
    vtkGenericWarningMacro("Unhandled sequence of value in state.");
    return false;
  }

  static bool willChangeWithValue(int curValue, bool hasLastValue, int lastValue, int lastSlope)
  {
    return hasChangedWithValue(curValue, hasLastValue, lastValue, lastSlope);
  }
};

//-----------------------------------------------------------------------------
class vtkVelodyneHDLReader::vtkInternal : public vtkLidarReaderInternal
{
public:
  vtkInternal(vtkVelodyneHDLReader* obj)
    : vtkLidarReaderInternal(obj)
  {
    this->Lidar = obj;
    this->RpmCalculator.Reset();
    this->AlreadyWarnAboutCalibration = false;
    this->UseIntraFiringAdjustment = true;
    this->ShouldAddDualReturnArray = false;
    this->alreadyWarnedForIgnoredHDL64FiringPacket = false;
    this->OutputPacketProcessingDebugInfo = false;
    this->SensorPowerMode = 0;
    this->CurrentFrameState = new FramingState;
    this->LastTimestamp = std::numeric_limits<unsigned int>::max();
    this->TimeAdjust = std::numeric_limits<double>::quiet_NaN();
    this->Skip = 0;
    this->FiringsSkip = 0;
    this->ShouldCheckSensor = true;

    std::fill(this->LastPointId, this->LastPointId + HDL_MAX_NUM_LASERS, -1);

    this->LaserSelection.resize(HDL_MAX_NUM_LASERS, true);
    this->DualReturnFilter = 0;
    this->IsHDL64Data = false;
    this->ReportedFactoryField1 = 0;
    this->ReportedFactoryField2 = 0;
    this->distanceResolutionM = 0.002;
    this->WantIntensityCorrection = false;

    this->rollingCalibrationData = new vtkRollingDataAccumulator();
    this->Init();
  }

  ~vtkInternal()
  {
    if (this->rollingCalibrationData)
    {
      delete this->rollingCalibrationData;
    }
    delete this->CurrentFrameState;
  }
  vtkVelodyneHDLReader* Lidar;
  vtkSmartPointer<vtkPolyData> CurrentDataset;

  vtkSmartPointer<vtkPoints> Points;
  vtkSmartPointer<vtkDoubleArray> PointsX;
  vtkSmartPointer<vtkDoubleArray> PointsY;
  vtkSmartPointer<vtkDoubleArray> PointsZ;
  vtkSmartPointer<vtkUnsignedCharArray> Intensity;
  vtkSmartPointer<vtkUnsignedCharArray> LaserId;
  vtkSmartPointer<vtkUnsignedShortArray> Azimuth;
  vtkSmartPointer<vtkDoubleArray> Distance;
  vtkSmartPointer<vtkUnsignedShortArray> DistanceRaw;
  vtkSmartPointer<vtkDoubleArray> Timestamp;
  vtkSmartPointer<vtkDoubleArray> VerticalAngle;
  vtkSmartPointer<vtkUnsignedIntArray> RawTime;
  vtkSmartPointer<vtkIntArray> IntensityFlag;
  vtkSmartPointer<vtkIntArray> DistanceFlag;
  vtkSmartPointer<vtkUnsignedIntArray> Flags;
  vtkSmartPointer<vtkIdTypeArray> DualReturnMatching;
  vtkSmartPointer<vtkDoubleArray> SelectedDualReturn;
  bool ShouldAddDualReturnArray;

  // sensor information
  bool HasDualReturn;
  SensorType ReportedSensor;
  DualReturnSensorMode ReportedSensorReturnMode;
  bool IsHDL64Data;
  uint8_t ReportedFactoryField1;
  uint8_t ReportedFactoryField2;

  bool alreadyWarnedForIgnoredHDL64FiringPacket;

  bool OutputPacketProcessingDebugInfo;

  // Bolean to manage the correction of intensity which indicates if the user want to correct the
  // intensities
  bool WantIntensityCorrection;
  bool ShouldCheckSensor;

  // WIP : We now have two method to compute the RPM :
  // - One method which computes the rpm using the point cloud
  // this method is not packets dependant but need a none empty
  // point cloud which can be tricky (hard cropping, none spinning sensor)
  // - One method which computes the rpm directly using the packets. the problem
  // is, if the packets format change, we will have to adapt the rpm computation
  // - For now, we will use the packet dependant method
  RPMCalculator RpmCalculator;

  FramingState* CurrentFrameState;
  unsigned int LastTimestamp;
  std::vector<double> RpmByFrames;
  double TimeAdjust;
  vtkIdType LastPointId[HDL_MAX_NUM_LASERS];
  vtkIdType FirstPointIdOfDualReturnPair;

  unsigned char SensorPowerMode;

  // Parameters ready by calibration
  std::vector<double> cos_lookup_table_;
  std::vector<double> sin_lookup_table_;
  HDLLaserCorrection laser_corrections_[HDL_MAX_NUM_LASERS];
  double XMLColorTable[HDL_MAX_NUM_LASERS][3];
  bool IsCorrectionFromLiveStream;

  // Sensor parameters presented as rolling data, extracted from enough packets
  vtkRollingDataAccumulator* rollingCalibrationData;

  // User configurable parameters
  std::vector<int> Skips;
  int Skip;
  int FiringsSkip;
  bool UseIntraFiringAdjustment;

  bool AlreadyWarnAboutCalibration;

  unsigned int DualReturnFilter;

  void SplitFrame(bool force = false);
  vtkSmartPointer<vtkPolyData> CreateData(vtkIdType numberOfPoints);
  vtkSmartPointer<vtkCellArray> NewVertexCells(vtkIdType numberOfVerts);

  void Init();
  void InitTrigonometricTables();
  void PrecomputeCorrectionCosSin();
  void LoadCalibration(const std::string& filename) override;
  bool HDL64LoadCorrectionsFromStreamData();

  void ProcessPacket(unsigned char* data, std::size_t bytesReceived) override;

  double ComputeTimestamp(unsigned int tohTime);
  void ComputeOrientation(double timestamp, vtkTransform* geotransform);

  // Process the laser return from the firing data
  // firingData - one of HDL_FIRING_PER_PKT from the packet
  // hdl64offset - either 0 or 32 to support 64-laser systems
  // firingBlock - block of packet for firing [0-11]
  // azimuthDiff - average azimuth change between firings
  // timestamp - the timestamp of the packet
  // geotransform - georeferencing transform
  void ProcessFiring(HDLFiringData* firingData, int hdl65offset, int firingBlock, int azimuthDiff,
    double timestamp, unsigned int rawtime, bool isThisFiringDualReturnData,
    bool isDualReturnPacket, vtkTransform* geotransform);

  void PushFiringData(const unsigned char laserId, const unsigned char rawLaserId,
    unsigned short azimuth, const double timestamp, const unsigned int rawtime,
    const HDLLaserReturn* laserReturn, const HDLLaserCorrection* correction,
    vtkTransform* geotransform, const bool isFiringDualReturnData);
  void ComputeCorrectedValues(const unsigned short azimuth, const HDLLaserReturn* laserReturn,
    const HDLLaserCorrection* correction, double pos[3], double& distanceM, short& intensity,
    bool correctIntensity);

  void UnloadPerFrameData() override;

  bool IsLidarPacket(const unsigned char *&data, unsigned int &dataLength, pcap_pkthdr **headerReference, unsigned int *dataHeaderLength) override;
  int CountNewFrameInPacket(const unsigned char *&data, unsigned int &dataLength, pcap_pkthdr **headerReference, unsigned int *dataHeaderLength) override;
  void CheckSensorCalibrationConsistency() override;
};

//-----------------------------------------------------------------------------
std::string vtkVelodyneHDLReader::GetSensorInformation()
{
  std::stringstream streamInfo;
  streamInfo << "Factory Field 1: " << (int)this->Internal->ReportedFactoryField1 << " (hex: 0x"
             << std::hex << (int)this->Internal->ReportedFactoryField1 << std::dec << " ) "
             << DataPacketFixedLength::DualReturnSensorModeToString(
                  static_cast<DualReturnSensorMode>(this->Internal->ReportedFactoryField1))
             << "  |  "
             << "Factory Field 2: " << (int)this->Internal->ReportedFactoryField2 << " (hex: 0x"
             << std::hex << (int)this->Internal->ReportedFactoryField2 << std::dec << " ) "
             << DataPacketFixedLength::SensorTypeToString(
                  static_cast<SensorType>(this->Internal->ReportedFactoryField2));

  return std::string(streamInfo.str());
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetFileName(const std::string &filename)
{
  if (filename == this->Internal->FileName)
  {
    return;
  }

  this->Internal->FileName = filename;
  this->Internal->FilePositions.clear();
  this->Internal->Skips.clear();
  this->Internal->UnloadPerFrameData();
  this->Modified();
}

//-----------------------------------------------------------------------------
bool vtkVelodyneHDLReader::vtkInternal::IsLidarPacket(const unsigned char *&data, unsigned int &dataLength, pcap_pkthdr **headerReference, unsigned int *dataHeaderLength)
{
  if (dataLength == HDLDataPacket::getDataByteLength() || dataLength == 512)
  {
    return true;
  }
  return false;
}

//-----------------------------------------------------------------------------
int vtkVelodyneHDLReader::vtkInternal::CountNewFrameInPacket(const unsigned char *&data, unsigned int &dataLength, pcap_pkthdr **headerReference, unsigned int *dataHeaderLength)
{
  // dont check for frame counts if it was not a firing packet
  if (dataLength != HDLDataPacket::getDataByteLength())
  {
    return 0;
  }
  FramingState currentFrameState;
  int count = 0;
  // Check if we cycled a frame and decrement
  const HDLDataPacket* dataPacket = reinterpret_cast<const HDLDataPacket*>(data);

  for (int i = this->Skip; i < HDL_FIRING_PER_PKT; ++i)
  {
    const HDLFiringData& firingData = dataPacket->firingData[i];

    if (currentFrameState.hasChangedWithValue(firingData))
    {
      count++;
    }
  }
  return count;
}

void vtkVelodyneHDLReader::vtkInternal::CheckSensorCalibrationConsistency()
{
  // check if the reported sensor is consistent with the calibration sensor
  if (this->ShouldCheckSensor)
  {
    const unsigned char* data;
    unsigned int dataLength;
    double timeSinceStart;
    // Open the .pcap
    this->Open();
    // set the position to the first full frame
    this->Reader->SetFilePosition(&this->FilePositions[0]);
    // Read the data of a packet
    while (this->Reader->NextPacket(data, dataLength, timeSinceStart))
    {
      // Update the sensor type if appropriate packet
      if (this->Lidar->updateReportedSensor(data, dataLength))
      {
        // Compare the number of lasers from calibration and from sensor
        this->Lidar->isReportedSensorAndCalibrationFileConsistent(true);
        // check is done
        this->ShouldCheckSensor = false;
        break;
      }
    }
    // close the .pcap file
    this->Close();
  }
}

//-----------------------------------------------------------------------------
vtkStandardNewMacro(vtkVelodyneHDLReader);

//-----------------------------------------------------------------------------
vtkVelodyneHDLReader::vtkVelodyneHDLReader() : vtkVelodyneHDLReader(new vtkInternal(this))
{
  SetPimpInternal(this->Internal);
}

//-----------------------------------------------------------------------------
vtkVelodyneHDLReader::vtkVelodyneHDLReader(vtkInternal* pimpl) : vtkLidarReader(new vtkInternal(this))
{
  this->Internal = pimpl;
//  this->UnloadPerFrameData();
  this->SetNumberOfInputPorts(0);
  this->SetNumberOfOutputPorts(1);
}

//-----------------------------------------------------------------------------
int vtkVelodyneHDLReader::GetOutputPacketProcessingDebugInfo() const
{
  return this->Internal->OutputPacketProcessingDebugInfo;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetOutputPacketProcessingDebugInfo(int value)
{
  if (this->Internal->OutputPacketProcessingDebugInfo != value)
  {
    this->Internal->OutputPacketProcessingDebugInfo = value;
    this->Modified();
  }
}

//-----------------------------------------------------------------------------
int vtkVelodyneHDLReader::GetIntraFiringAdjust() const
{
  return this->Internal->UseIntraFiringAdjustment;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetIntraFiringAdjust(int value)
{
  if (this->Internal->UseIntraFiringAdjustment != value)
  {
    this->Internal->UseIntraFiringAdjustment = value;
    this->Modified();
  }
}

//-----------------------------------------------------------------------------
unsigned int vtkVelodyneHDLReader::GetDualReturnFilter() const
{
  return this->Internal->DualReturnFilter;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetDualReturnFilter(unsigned int filter)
{
  if (this->Internal->DualReturnFilter != filter)
  {
    this->Internal->DualReturnFilter = filter;
    this->Modified();
  }
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::GetLaserCorrections(double verticalCorrection[HDL_MAX_NUM_LASERS],
  double rotationalCorrection[HDL_MAX_NUM_LASERS], double distanceCorrection[HDL_MAX_NUM_LASERS],
  double distanceCorrectionX[HDL_MAX_NUM_LASERS], double distanceCorrectionY[HDL_MAX_NUM_LASERS],
  double verticalOffsetCorrection[HDL_MAX_NUM_LASERS],
  double horizontalOffsetCorrection[HDL_MAX_NUM_LASERS], double focalDistance[HDL_MAX_NUM_LASERS],
  double focalSlope[HDL_MAX_NUM_LASERS], double minIntensity[HDL_MAX_NUM_LASERS],
  double maxIntensity[HDL_MAX_NUM_LASERS])
{
  for (int i = 0; i < HDL_MAX_NUM_LASERS; ++i)
  {
    verticalCorrection[i] = this->Internal->laser_corrections_[i].verticalCorrection;
    rotationalCorrection[i] = this->Internal->laser_corrections_[i].rotationalCorrection;
    distanceCorrection[i] = this->Internal->laser_corrections_[i].distanceCorrection;
    distanceCorrectionX[i] = this->Internal->laser_corrections_[i].distanceCorrectionX;
    distanceCorrectionY[i] = this->Internal->laser_corrections_[i].distanceCorrectionY;
    verticalOffsetCorrection[i] = this->Internal->laser_corrections_[i].verticalOffsetCorrection;
    horizontalOffsetCorrection[i] =
      this->Internal->laser_corrections_[i].horizontalOffsetCorrection;
    focalDistance[i] = this->Internal->laser_corrections_[i].focalDistance;
    focalSlope[i] = this->Internal->laser_corrections_[i].focalSlope;
    minIntensity[i] = this->Internal->laser_corrections_[i].minIntensity;
    maxIntensity[i] = this->Internal->laser_corrections_[i].maxIntensity;
  }
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::GetXMLColorTable(double XMLColorTable[4 * HDL_MAX_NUM_LASERS])
{
  for (int i = 0; i < HDL_MAX_NUM_LASERS; ++i)
  {
    XMLColorTable[i * 4] = static_cast<double>(i) / 63.0 * 255.0;
    for (int j = 0; j < 3; ++j)
    {
      XMLColorTable[i * 4 + j + 1] = this->Internal->XMLColorTable[i][j];
    }
  }
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetFiringsSkip(int pr)
{
  this->Internal->FiringsSkip = pr;
  this->Modified();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetCalibrationFileName(const std::string& filename)
{
  if (filename.empty())
  {
    // no calibration file: HDL64 with autocalibration
    this->Internal->IsCalibrated = false;
    this->Internal->IsCorrectionFromLiveStream = true;
  }
  else
  {
    vtkLidarProvider::SetCalibrationFileName(filename);
    this->Internal->IsCorrectionFromLiveStream = false;
  }
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::UnloadPerFrameData()
{
  std::fill(this->LastPointId, this->LastPointId + HDL_MAX_NUM_LASERS, -1);
  this->CurrentFrameState->reset();
  this->LastTimestamp = std::numeric_limits<unsigned int>::max();
  this->TimeAdjust = std::numeric_limits<double>::quiet_NaN();

  this->rollingCalibrationData->clear();
  this->HasDualReturn = false;
  this->IsHDL64Data = false;
  this->Datasets.clear();
  this->CurrentDataset = this->CreateData(0);

  this->ShouldCheckSensor = true;
}

//-----------------------------------------------------------------------------
vtkSmartPointer<vtkPolyData> vtkVelodyneHDLReader::GetFrame(int frameNumber, int wantedNumberOfTrailingFrames)
{
  // Setup some variable needed by ProcessPacket
  this->Internal->Skip = this->Internal->Skips[frameNumber];
  vtkSmartPointer<vtkPolyData> output = vtkLidarReader::GetFrame(frameNumber, wantedNumberOfTrailingFrames);
  if (this->Internal->ShouldAddDualReturnArray)
  {
    output->GetPointData()->AddArray(this->Internal->SelectedDualReturn);
  }
  return output;
}

//-----------------------------------------------------------------------------
int vtkVelodyneHDLReader::RequestInformation(
  vtkInformation* request, vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  if (this->Internal->FileName.length() &&
    (!this->Internal->FilePositions.size() || this->Internal->IsCorrectionFromLiveStream))
  {
    this->ReadFrameInformation();
  }
  return this->vtkLidarReader::RequestInformation(request, inputVector, outputVector);
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
  os << indent << "FileName: " << this->Internal->FileName << endl;
  os << indent << "CalibrationFile: " << this->Internal->CalibrationFileName << endl;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetShouldAddDualReturnArray(bool input)
{
  this->Internal->ShouldAddDualReturnArray = input;
}

//-----------------------------------------------------------------------------
std::vector<vtkSmartPointer<vtkPolyData> >& vtkVelodyneHDLReader::GetDatasets()
{
  return this->Internal->Datasets;
}

namespace
{
template<typename T>
vtkSmartPointer<T> CreateDataArray(const char* name, vtkIdType np, vtkPolyData* pd)
{
  vtkSmartPointer<T> array = vtkSmartPointer<T>::New();
  array->Allocate(60000);
  array->SetName(name);
  array->SetNumberOfTuples(np);

  if (pd)
  {
    pd->GetPointData()->AddArray(array);
  }

  return array;
}
}

#define PacketProcessingDebugMacro(x)                                                              \
  {                                                                                                \
    if (this->Internal->OutputPacketProcessingDebugInfo)                                           \
    {                                                                                              \
      std::cout << " " x;                                                                          \
    }                                                                                              \
  }

//-----------------------------------------------------------------------------
vtkSmartPointer<vtkPolyData> vtkVelodyneHDLReader::vtkInternal::CreateData(vtkIdType numberOfPoints)
{
  vtkSmartPointer<vtkPolyData> polyData = vtkSmartPointer<vtkPolyData>::New();

  // points
  vtkNew<vtkPoints> points;
  points->SetDataTypeToFloat();
  points->Allocate(60000);
  points->SetNumberOfPoints(numberOfPoints);
  points->GetData()->SetName("Points_m_XYZ");
  polyData->SetPoints(points.GetPointer());
  polyData->SetVerts(NewVertexCells(numberOfPoints));

  // intensity
  this->Points = points.GetPointer();
  this->PointsX = CreateDataArray<vtkDoubleArray>("X", numberOfPoints, polyData);
  this->PointsY = CreateDataArray<vtkDoubleArray>("Y", numberOfPoints, polyData);
  this->PointsZ = CreateDataArray<vtkDoubleArray>("Z", numberOfPoints, polyData);
  this->Intensity = CreateDataArray<vtkUnsignedCharArray>("intensity", numberOfPoints, polyData);
  this->LaserId = CreateDataArray<vtkUnsignedCharArray>("laser_id", numberOfPoints, polyData);
  this->Azimuth = CreateDataArray<vtkUnsignedShortArray>("azimuth", numberOfPoints, polyData);
  this->Distance = CreateDataArray<vtkDoubleArray>("distance_m", numberOfPoints, polyData);
  this->DistanceRaw =
    CreateDataArray<vtkUnsignedShortArray>("distance_raw", numberOfPoints, polyData);
  this->Timestamp = CreateDataArray<vtkDoubleArray>("adjustedtime", numberOfPoints, polyData);
  this->RawTime = CreateDataArray<vtkUnsignedIntArray>("timestamp", numberOfPoints, polyData);
  this->DistanceFlag = CreateDataArray<vtkIntArray>("dual_distance", numberOfPoints, 0);
  this->IntensityFlag = CreateDataArray<vtkIntArray>("dual_intensity", numberOfPoints, 0);
  this->Flags = CreateDataArray<vtkUnsignedIntArray>("dual_flags", numberOfPoints, 0);
  this->DualReturnMatching =
    CreateDataArray<vtkIdTypeArray>("dual_return_matching", numberOfPoints, 0);
  this->VerticalAngle = CreateDataArray<vtkDoubleArray>("vertical_angle", numberOfPoints, polyData);

  // FieldData : RPM
  vtkSmartPointer<vtkDoubleArray> rpmData = vtkSmartPointer<vtkDoubleArray>::New();
  rpmData->SetNumberOfTuples(1);     // One tuple
  rpmData->SetNumberOfComponents(1); // One value per tuple, the scalar
  rpmData->SetName("RotationPerMinute");
  rpmData->SetTuple1(0, this->Frequency);
  polyData->GetFieldData()->AddArray(rpmData);

  if (this->HasDualReturn)
  {
    polyData->GetPointData()->AddArray(this->DistanceFlag.GetPointer());
    polyData->GetPointData()->AddArray(this->IntensityFlag.GetPointer());
    polyData->GetPointData()->AddArray(this->DualReturnMatching.GetPointer());
  }

  return polyData;
}

//----------------------------------------------------------------------------
vtkSmartPointer<vtkCellArray> vtkVelodyneHDLReader::vtkInternal::NewVertexCells(
  vtkIdType numberOfVerts)
{
  vtkNew<vtkIdTypeArray> cells;
  cells->SetNumberOfValues(numberOfVerts * 2);
  vtkIdType* ids = cells->GetPointer(0);
  for (vtkIdType i = 0; i < numberOfVerts; ++i)
  {
    ids[i * 2] = 1;
    ids[i * 2 + 1] = i;
  }

  vtkSmartPointer<vtkCellArray> cellArray = vtkSmartPointer<vtkCellArray>::New();
  cellArray->SetCells(numberOfVerts, cells.GetPointer());
  return cellArray;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::PushFiringData(const unsigned char laserId,
  const unsigned char rawLaserId, unsigned short azimuth, const double timestamp,
  const unsigned int rawtime, const HDLLaserReturn* laserReturn,
  const HDLLaserCorrection* correction, vtkTransform* geotransform,
  const bool isFiringDualReturnData)
{
  azimuth %= 36000;
  const vtkIdType thisPointId = this->Points->GetNumberOfPoints();
  short intensity = laserReturn->intensity;

  // Compute raw position
  double distanceM;
  double pos[3];
  bool applyIntensityCorrection =
    this->WantIntensityCorrection && this->IsHDL64Data && !(this->SensorPowerMode == CorrectionOn);
  ComputeCorrectedValues(
    azimuth, laserReturn, correction, pos, distanceM, intensity, applyIntensityCorrection);

  // Apply sensor transform
  this->SensorTransform->InternalTransformPoint(pos, pos);

  if (this->shouldBeCroppedOut(pos, static_cast<double>(azimuth) / 100.0))
    return;

  // Do not add any data before here as this might short-circuit
  if (isFiringDualReturnData)
  {
    const vtkIdType dualPointId = this->LastPointId[rawLaserId];
    if (dualPointId < this->FirstPointIdOfDualReturnPair)
    {
      // No matching point from first set (skipped?)
      this->Flags->InsertNextValue(DUAL_DOUBLED);
      this->DistanceFlag->InsertNextValue(0);
      this->DualReturnMatching->InsertNextValue(-1); // std::numeric_limits<vtkIdType>::quiet_NaN()
      this->IntensityFlag->InsertNextValue(0);
    }
    else
    {
      const short dualIntensity = this->Intensity->GetValue(dualPointId);
      const double dualDistance = this->Distance->GetValue(dualPointId);
      unsigned int firstFlags = this->Flags->GetValue(dualPointId);
      unsigned int secondFlags = 0;

      if (dualDistance == distanceM && intensity == dualIntensity)
      {
        // ignore duplicate point and leave first with original flags
        return;
      }

      if (dualIntensity < intensity)
      {
        firstFlags &= ~DUAL_INTENSITY_HIGH;
        secondFlags |= DUAL_INTENSITY_HIGH;
      }
      else
      {
        firstFlags &= ~DUAL_INTENSITY_LOW;
        secondFlags |= DUAL_INTENSITY_LOW;
      }

      if (dualDistance < distanceM)
      {
        firstFlags &= ~DUAL_DISTANCE_FAR;
        secondFlags |= DUAL_DISTANCE_FAR;
      }
      else
      {
        firstFlags &= ~DUAL_DISTANCE_NEAR;
        secondFlags |= DUAL_DISTANCE_NEAR;
      }

      // We will output only one point so return out of this
      if (this->DualReturnFilter)
      {
        if (!(secondFlags & this->DualReturnFilter))
        {
          // second return does not match filter; skip
          this->Flags->SetValue(dualPointId, firstFlags);
          this->DistanceFlag->SetValue(dualPointId, MapDistanceFlag(firstFlags));
          this->IntensityFlag->SetValue(dualPointId, MapIntensityFlag(firstFlags));
          return;
        }
        if (!(firstFlags & this->DualReturnFilter))
        {
          // first return does not match filter; replace with second return
          // Apply geoposition transform
          geotransform->InternalTransformPoint(pos, pos);
          this->Points->SetPoint(dualPointId, pos);
          this->Distance->SetValue(dualPointId, distanceM);
          this->DistanceRaw->SetValue(dualPointId, laserReturn->distance);
          this->Intensity->SetValue(dualPointId, intensity);
          this->Timestamp->SetValue(dualPointId, timestamp);
          this->RawTime->SetValue(dualPointId, rawtime);
          this->Flags->SetValue(dualPointId, secondFlags);
          this->DistanceFlag->SetValue(dualPointId, MapDistanceFlag(secondFlags));
          this->IntensityFlag->SetValue(dualPointId, MapIntensityFlag(secondFlags));
          return;
        }
      }

      this->Flags->SetValue(dualPointId, firstFlags);
      this->DistanceFlag->SetValue(dualPointId, MapDistanceFlag(firstFlags));
      this->IntensityFlag->SetValue(dualPointId, MapIntensityFlag(firstFlags));
      this->Flags->InsertNextValue(secondFlags);
      this->DistanceFlag->InsertNextValue(MapDistanceFlag(secondFlags));
      this->IntensityFlag->InsertNextValue(MapIntensityFlag(secondFlags));
      // The first return indicates the dual return
      // and the dual return indicates the first return
      this->DualReturnMatching->InsertNextValue(dualPointId);
      this->DualReturnMatching->SetValue(dualPointId, thisPointId);
    }
  }
  else
  {
    this->Flags->InsertNextValue(DUAL_DOUBLED);
    this->DistanceFlag->InsertNextValue(0);
    this->IntensityFlag->InsertNextValue(0);
    this->DualReturnMatching->InsertNextValue(-1); // std::numeric_limits<vtkIdType>::quiet_NaN()
  }

  // Apply geoposition transform
  geotransform->InternalTransformPoint(pos, pos);
  this->Points->InsertNextPoint(pos);
  this->PointsX->InsertNextValue(pos[0]);
  this->PointsY->InsertNextValue(pos[1]);
  this->PointsZ->InsertNextValue(pos[2]);
  this->Azimuth->InsertNextValue(azimuth);
  this->Intensity->InsertNextValue(intensity);
  this->LaserId->InsertNextValue(laserId);
  this->Timestamp->InsertNextValue(timestamp);
  this->RawTime->InsertNextValue(rawtime);
  this->Distance->InsertNextValue(distanceM);
  this->DistanceRaw->InsertNextValue(laserReturn->distance);
  this->LastPointId[rawLaserId] = thisPointId;
  this->VerticalAngle->InsertNextValue(this->laser_corrections_[laserId].verticalCorrection);
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::InitTrigonometricTables()
{
  if (cos_lookup_table_.size() == 0 || sin_lookup_table_.size() == 0)
  {
    cos_lookup_table_.resize(HDL_NUM_ROT_ANGLES);
    sin_lookup_table_.resize(HDL_NUM_ROT_ANGLES);
    for (unsigned int i = 0; i < HDL_NUM_ROT_ANGLES; i++)
    {
      double rad = HDL_Grabber_toRadians(i / 100.0);
      cos_lookup_table_[i] = std::cos(rad);
      sin_lookup_table_[i] = std::sin(rad);
    }
  }
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::LoadCalibration(const std::string& filename)
{
  boost::property_tree::ptree pt;
  try
  {
    read_xml(filename, pt, boost::property_tree::xml_parser::trim_whitespace);
  }
  catch (boost::exception const&)
  {
    vtkGenericWarningMacro(
      "LoadCalibration: error reading calibration file: " << filename);
    return;
  }
  // Read distLSB if provided
  BOOST_FOREACH (boost::property_tree::ptree::value_type& v, pt.get_child("boost_serialization.DB"))
  {
    if (v.first == "distLSB_")
    { // Stored in cm in xml
      distanceResolutionM = atof(v.second.data().c_str()) / 100.0;
    }
  }

  int i, j;
  i = 0;
  BOOST_FOREACH (
    boost::property_tree::ptree::value_type& p, pt.get_child("boost_serialization.DB.colors_"))
  {
    if (p.first == "item")
    {
      j = 0;
      BOOST_FOREACH (boost::property_tree::ptree::value_type& v, p.second.get_child("rgb"))
        if (v.first == "item")
        {
          std::stringstream ss;
          double val;
          ss << v.second.data();
          ss >> val;

          XMLColorTable[i][j] = val;
          j++;
        }
      i++;
    }
  }

  int enabledCount = 0;
  BOOST_FOREACH (
    boost::property_tree::ptree::value_type& v, pt.get_child("boost_serialization.DB.enabled_"))
  {
    std::stringstream ss;
    if (v.first == "item")
    {
      ss << v.second.data();
      int test = 0;
      ss >> test;
      if (!ss.fail() && test == 1)
      {
        enabledCount++;
      }
    }
  }
  this->CalibrationReportedNumLasers = enabledCount;

  // Getting min & max intensities from XML
  int laserId = 0;
  int minIntensity[HDL_MAX_NUM_LASERS], maxIntensity[HDL_MAX_NUM_LASERS];
  BOOST_FOREACH (boost::property_tree::ptree::value_type& v,
    pt.get_child("boost_serialization.DB.minIntensity_"))
  {
    std::stringstream ss;
    if (v.first == "item")
    {
      ss << v.second.data();
      ss >> minIntensity[laserId];
      laserId++;
    }
  }

  laserId = 0;
  BOOST_FOREACH (boost::property_tree::ptree::value_type& v,
    pt.get_child("boost_serialization.DB.maxIntensity_"))
  {
    std::stringstream ss;
    if (v.first == "item")
    {
      ss << v.second.data();
      ss >> maxIntensity[laserId];
      laserId++;
    }
  }

  BOOST_FOREACH (
    boost::property_tree::ptree::value_type& v, pt.get_child("boost_serialization.DB.points_"))
  {
    if (v.first == "item")
    {
      boost::property_tree::ptree points = v.second;
      BOOST_FOREACH (boost::property_tree::ptree::value_type& px, points)
      {
        if (px.first == "px")
        {
          boost::property_tree::ptree calibrationData = px.second;
          int index = -1;
          HDLLaserCorrection xmlData;

          BOOST_FOREACH (boost::property_tree::ptree::value_type& item, calibrationData)
          {
            if (item.first == "id_")
              index = atoi(item.second.data().c_str());
            if (item.first == "rotCorrection_")
              xmlData.rotationalCorrection = atof(item.second.data().c_str());
            if (item.first == "vertCorrection_")
              xmlData.verticalCorrection = atof(item.second.data().c_str());
            if (item.first == "distCorrection_")
              xmlData.distanceCorrection = atof(item.second.data().c_str());
            if (item.first == "distCorrectionX_")
              xmlData.distanceCorrectionX = atof(item.second.data().c_str());
            if (item.first == "distCorrectionY_")
              xmlData.distanceCorrectionY = atof(item.second.data().c_str());
            if (item.first == "vertOffsetCorrection_")
              xmlData.verticalOffsetCorrection = atof(item.second.data().c_str());
            if (item.first == "horizOffsetCorrection_")
              xmlData.horizontalOffsetCorrection = atof(item.second.data().c_str());
            if (item.first == "focalDistance_")
              xmlData.focalDistance = atof(item.second.data().c_str());
            if (item.first == "focalSlope_")
              xmlData.focalSlope = atof(item.second.data().c_str());
            if (item.first == "closeSlope_")
              xmlData.closeSlope = atof(item.second.data().c_str());
          }
          if (index != -1 && index < HDL_MAX_NUM_LASERS)
          {
            laser_corrections_[index] = xmlData;
            // Angles are already stored in degrees in xml
            // Distances are stored in centimeters in xml, and we store meters.
            laser_corrections_[index].distanceCorrection /= 100.0;
            laser_corrections_[index].distanceCorrectionX /= 100.0;
            laser_corrections_[index].distanceCorrectionY /= 100.0;
            laser_corrections_[index].verticalOffsetCorrection /= 100.0;
            laser_corrections_[index].horizontalOffsetCorrection /= 100.0;
            laser_corrections_[index].focalDistance /= 100.0;
            laser_corrections_[index].focalSlope /= 100.0;
            laser_corrections_[index].closeSlope /= 100.0;
            if (laser_corrections_[index].closeSlope == 0.0)
              laser_corrections_[index].closeSlope = laser_corrections_[index].focalSlope;
            laser_corrections_[index].minIntensity = minIntensity[index];
            laser_corrections_[index].maxIntensity = maxIntensity[index];
          }
        }
      }
    }
  }

  int idx = 0;
  BOOST_FOREACH (boost::property_tree::ptree::value_type& v,
    pt.get_child("boost_serialization.DB.minIntensity_"))
  {
    std::stringstream ss;
    if (v.first == "item")
    {
      ss << v.second.data();
      int intensity = 0;
      ss >> intensity;
      if (!ss.fail() && idx < HDL_MAX_NUM_LASERS)
      {
        laser_corrections_[idx].minIntensity = intensity;
      }
      idx++;
    }
  }

  idx = 0;
  BOOST_FOREACH (boost::property_tree::ptree::value_type& v,
    pt.get_child("boost_serialization.DB.maxIntensity_"))
  {
    std::stringstream ss;
    if (v.first == "item")
    {
      ss << v.second.data();
      int intensity = 0;
      ss >> intensity;
      if (!ss.fail() && idx < HDL_MAX_NUM_LASERS)
      {
        laser_corrections_[idx].maxIntensity = intensity;
      }
      idx++;
    }
  }

  PrecomputeCorrectionCosSin();
  this->IsCalibrated = true;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::PrecomputeCorrectionCosSin()
{

  for (int i = 0; i < HDL_MAX_NUM_LASERS; i++)
  {
    HDLLaserCorrection& correction = laser_corrections_[i];
    correction.cosVertCorrection = std::cos(HDL_Grabber_toRadians(correction.verticalCorrection));
    correction.sinVertCorrection = std::sin(HDL_Grabber_toRadians(correction.verticalCorrection));
    correction.cosRotationalCorrection =
      std::cos(HDL_Grabber_toRadians(correction.rotationalCorrection));
    correction.sinRotationalCorrection =
      std::sin(HDL_Grabber_toRadians(correction.rotationalCorrection));
    correction.sinVertOffsetCorrection =
      correction.verticalOffsetCorrection * correction.sinVertCorrection;
    correction.cosVertOffsetCorrection =
      correction.verticalOffsetCorrection * correction.cosVertCorrection;
  }
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::Init()
{
  this->InitTrigonometricTables();
  this->SensorTransform->Identity();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::SplitFrame(bool force)
{
  if ((this->IgnoreEmptyFrames && this->CurrentDataset->GetNumberOfPoints() == 0) && !force)
  {
    return;
  }

  if (this->SplitCounter > 0 && !force)
  {
    this->SplitCounter--;
    return;
  }

  for (size_t n = 0; n < HDL_MAX_NUM_LASERS; ++n)
  {
    this->LastPointId[n] = -1;
  }

  this->CurrentDataset->SetVerts(this->NewVertexCells(this->CurrentDataset->GetNumberOfPoints()));

  // Compute the rpm and reset
  this->Frequency = this->RpmCalculator.GetRPM();
  this->RpmCalculator.Reset();

  this->CurrentDataset->GetFieldData()
    ->GetArray("RotationPerMinute")
    ->SetTuple1(0, this->Frequency);
  this->Datasets.push_back(this->CurrentDataset);
  this->CurrentDataset = this->CreateData(0);
}

//-----------------------------------------------------------------------------
double vtkVelodyneHDLReader::vtkInternal::ComputeTimestamp(unsigned int tohTime)
{
  static const double hourInMilliseconds = 3600.0 * 1e6;

  if (tohTime < this->LastTimestamp)
  {
    if (!vtkMath::IsFinite(this->TimeAdjust))
    {
      // First adjustment; must compute adjustment number
      if (this->Interp && this->Interp->GetNumberOfTransforms())
      {
        const double ts = static_cast<double>(tohTime) * 1e-6;
        const double hours = (this->Interp->GetMinimumT() - ts) / 3600.0;
        this->TimeAdjust = vtkMath::Round(hours) * hourInMilliseconds;
      }
      else
      {
        // Ought to warn about this, but happens when applogic is checking that
        // we can read the file :-(
        this->TimeAdjust = 0;
      }
    }
    else
    {
      // Hour has wrapped; add an hour to the update adjustment value
      this->TimeAdjust += hourInMilliseconds;
    }
  }

  this->LastTimestamp = tohTime;
  return static_cast<double>(tohTime) + this->TimeAdjust;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::ComputeOrientation(
  double timestamp, vtkTransform* geotransform)
{
  if (this->ApplyTransform && this->Interp && this->Interp->GetNumberOfTransforms())
  {
    // NOTE: We store time in milliseconds, but the interpolator uses seconds,
    //       so we need to adjust here
    const double t = timestamp * 1e-6;
    this->Interp->InterpolateTransform(t, geotransform);
  }
  else
  {
    geotransform->Identity();
  }
}

void vtkVelodyneHDLReader::vtkInternal::ComputeCorrectedValues(const unsigned short azimuth,
  const HDLLaserReturn* laserReturn, const HDLLaserCorrection* correction, double pos[3],
  double& distanceM, short& intensity, bool correctIntensity)
{
  intensity = laserReturn->intensity;

  double cosAzimuth, sinAzimuth;
  if (correction->rotationalCorrection == 0)
  {
    cosAzimuth = this->cos_lookup_table_[azimuth];
    sinAzimuth = this->sin_lookup_table_[azimuth];
  }
  else
  {
    // realAzimuth = azimuth/100 - rotationalCorrection
    // cos(a-b) = cos(a)*cos(b) + sin(a)*sin(b)
    // sin(a-b) = sin(a)*cos(b) - cos(a)*sin(b)
    cosAzimuth = this->cos_lookup_table_[azimuth] * correction->cosRotationalCorrection +
      this->sin_lookup_table_[azimuth] * correction->sinRotationalCorrection;
    sinAzimuth = this->sin_lookup_table_[azimuth] * correction->cosRotationalCorrection -
      this->cos_lookup_table_[azimuth] * correction->sinRotationalCorrection;
  }
  // Compute the distance in the xy plane (w/o accounting for rotation)
  /**the new term of 'vert_offset * sin_vert_angle'
   * was added to the expression due to the mathemathical
   * model we used.
   */
  double distanceMRaw = laserReturn->distance * this->distanceResolutionM;
  distanceM = distanceMRaw + correction->distanceCorrection;
  double xyDistance =
    distanceM * correction->cosVertCorrection - correction->sinVertOffsetCorrection;

  pos[0] = xyDistance * sinAzimuth - correction->horizontalOffsetCorrection * cosAzimuth;
  pos[1] = xyDistance * cosAzimuth + correction->horizontalOffsetCorrection * sinAzimuth;
  pos[2] = distanceM * correction->sinVertCorrection + correction->verticalOffsetCorrection;

  if (correctIntensity && (correction->minIntensity < correction->maxIntensity))
  {
    // Compute corrected intensity

    /* Please refer to the manual:
      "Velodyne, Inc. ©2013  63‐HDL64ES3 REV G" Appendix F. Pages 45-46
      PLease note: in the manual, focalDistance is in centimeters, distance is the raw short from
      the laser
      & the graph is in meter */

    // Casting the input values to double for the computation

    double computedIntensity = static_cast<double>(intensity);
    double minIntensity = static_cast<double>(correction->minIntensity);
    double maxIntensity = static_cast<double>(correction->maxIntensity);

    // Rescale the intensity between 0 and 255
    computedIntensity = (computedIntensity - minIntensity) / (maxIntensity - minIntensity) * 255.0;

    if (computedIntensity < 0)
    {
      computedIntensity = 0;
    }

    double focalOffset = 256 * pow(1.0 - correction->focalDistance / 131.0, 2);
    double insideAbsValue = std::abs(
      focalOffset - 256 * pow(1.0 - static_cast<double>(laserReturn->distance) / 65535.0f, 2));

    if (insideAbsValue > 0)
    {
      computedIntensity = computedIntensity + correction->focalSlope * insideAbsValue;
    }
    else
    {
      computedIntensity = computedIntensity + correction->closeSlope * insideAbsValue;
    }
    computedIntensity = std::max(std::min(computedIntensity, 255.0), 1.0);

    intensity = static_cast<short>(computedIntensity);
  }
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::ProcessFiring(HDLFiringData* firingData,
  int firingBlockLaserOffset, int firingBlock, int azimuthDiff, double timestamp,
  unsigned int rawtime, bool isThisFiringDualReturnData, bool isDualReturnPacket,
  vtkTransform* geotransform)
{
  // Non dual return piece of a dual return packet: init last point of laser
  if (!isThisFiringDualReturnData &&
    (!this->IsHDL64Data || (this->IsHDL64Data && ((firingBlock % 4) == 0))))
  {
    this->FirstPointIdOfDualReturnPair = this->Points->GetNumberOfPoints();
  }

  for (int dsr = 0; dsr < HDL_LASER_PER_FIRING; dsr++)
  {
    const unsigned char rawLaserId = static_cast<unsigned char>(dsr + firingBlockLaserOffset);
    unsigned char laserId = rawLaserId;
    const unsigned short azimuth = firingData->rotationalPosition;

    // Detect VLP-16 data and adjust laser id if necessary
    int firingWithinBlock = 0;

    if (this->CalibrationReportedNumLasers == 16)
    {
      if (firingBlockLaserOffset != 0)
      {
        if (!this->alreadyWarnedForIgnoredHDL64FiringPacket)
        {
          vtkGenericWarningMacro("Error: Received a HDL-64 UPPERBLOCK firing packet "
                                 "with a VLP-16 calibration file. Ignoring the firing.");
          this->alreadyWarnedForIgnoredHDL64FiringPacket = true;
        }
        return;
      }
      if (laserId >= 16)
      {
        laserId -= 16;
        firingWithinBlock = 1;
      }
    }

    // Interpolate azimuths and timestamps per laser within firing blocks
    double timestampadjustment = 0;
    int azimuthadjustment = 0;
    if (this->UseIntraFiringAdjustment)
    {
      double blockdsr0 = 0, nextblockdsr0 = 1;
      switch (this->CalibrationReportedNumLasers)
      {
        case 64:
        {
          timestampadjustment = -HDL64EAdjustTimeStamp(firingBlock, dsr, isDualReturnPacket);
          nextblockdsr0 = -HDL64EAdjustTimeStamp(
            firingBlock + (isDualReturnPacket ? 4 : 2), 0, isDualReturnPacket);
          blockdsr0 = -HDL64EAdjustTimeStamp(firingBlock, 0, isDualReturnPacket);
          break;
        }
        case 32:
        {
          if (this->ReportedSensor == VLP32AB || this->ReportedSensor == VLP32C)
          {
            timestampadjustment = VLP32AdjustTimeStamp(firingBlock, dsr, isDualReturnPacket);
            nextblockdsr0 = VLP32AdjustTimeStamp(
              firingBlock + (isDualReturnPacket ? 2 : 1), 0, isDualReturnPacket);
            blockdsr0 = VLP32AdjustTimeStamp(firingBlock, 0, isDualReturnPacket);
          }
          else if (this->ReportedSensor == HDL32E)
          {
            timestampadjustment = HDL32AdjustTimeStamp(firingBlock, dsr, isDualReturnPacket);
            nextblockdsr0 = HDL32AdjustTimeStamp(
              firingBlock + (isDualReturnPacket ? 2 : 1), 0, isDualReturnPacket);
            blockdsr0 = HDL32AdjustTimeStamp(firingBlock, 0, isDualReturnPacket);
          }
          break;
        }
        case 16:
        {
          timestampadjustment =
            VLP16AdjustTimeStamp(firingBlock, laserId, firingWithinBlock, isDualReturnPacket);
          nextblockdsr0 = VLP16AdjustTimeStamp(
            firingBlock + (isDualReturnPacket ? 2 : 1), 0, 0, isDualReturnPacket);
          blockdsr0 = VLP16AdjustTimeStamp(firingBlock, 0, 0, isDualReturnPacket);
          break;
        }
        default:
        {
          timestampadjustment = 0.0;
          blockdsr0 = 0.0;
          nextblockdsr0 = 1.0;
        }
      }
      azimuthadjustment = vtkMath::Round(
        azimuthDiff * ((timestampadjustment - blockdsr0) / (nextblockdsr0 - blockdsr0)));
      timestampadjustment = vtkMath::Round(timestampadjustment);
    }
    if ((!this->IgnoreZeroDistances || firingData->laserReturns[dsr].distance != 0.0) &&
      this->LaserSelection[laserId])
    {
      this->PushFiringData(laserId, rawLaserId, azimuth + azimuthadjustment,
        timestamp + timestampadjustment, rawtime + static_cast<unsigned int>(timestampadjustment),
        &(firingData->laserReturns[dsr]), &(laser_corrections_[dsr + firingBlockLaserOffset]),
        geotransform, isThisFiringDualReturnData);
    }
  }
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::vtkInternal::ProcessPacket(
  unsigned char* data, std::size_t bytesReceived)
{
  if (bytesReceived != 1206)
  {
    // Data-Packet Specifications says that laser-packets are 1206 byte long.
    //  That is : (2+2+(2+1)*32)*12 + 4 + 1 + 1
    //                #lasers^   ^#firingPerPkt
    return;
  }

  HDLDataPacket* dataPacket = reinterpret_cast<HDLDataPacket*>(data);

  vtkNew<vtkTransform> geotransform;
  const unsigned int rawtime = dataPacket->gpsTimestamp;
  const double timestamp = this->ComputeTimestamp(dataPacket->gpsTimestamp);
  this->ComputeOrientation(timestamp, geotransform.GetPointer());

  // Update the rpm computation (by packets)
  this->RpmCalculator.AddData(dataPacket, rawtime);

  // Update the transforms here and then call internal
  // transform
  this->SensorTransform->Update();
  geotransform->Update();

  int firingBlock = this->Skip;
  this->Skip = 0;

  // Compute the total azimuth advanced during one full firing block
  std::vector<int> diffs(HDL_FIRING_PER_PKT - 1);
  for (int i = 0; i < HDL_FIRING_PER_PKT - 1; ++i)
  {
    int localDiff = (36000 + dataPacket->firingData[i + 1].rotationalPosition -
                      dataPacket->firingData[i].rotationalPosition) %
      36000;
    diffs[i] = localDiff;
  }

  this->IsHDL64Data |= dataPacket->isHDL64();

  if (!IsHDL64Data)
  { // with HDL64, it should be filled by LoadCorrectionsFromStreamData
    this->ReportedSensor = dataPacket->getSensorType();
    this->ReportedSensorReturnMode = dataPacket->getDualReturnSensorMode();
  }

  std::sort(diffs.begin(), diffs.end());
  // Assume the median of the packet's rotationalPosition differences
  int azimuthDiff = diffs[HDL_FIRING_PER_PKT / 2];
  if (this->IsHDL64Data)
  {
    azimuthDiff = diffs[HDL_FIRING_PER_PKT - 2];
  }
  // assert(azimuthDiff > 0);

  // Add DualReturn-specific arrays if newly detected dual return packet
  if (dataPacket->isDualModeReturn(this->IsHDL64Data) && !this->HasDualReturn)
  {
    this->HasDualReturn = true;
    this->CurrentDataset->GetPointData()->AddArray(this->DistanceFlag.GetPointer());
    this->CurrentDataset->GetPointData()->AddArray(this->IntensityFlag.GetPointer());
    this->CurrentDataset->GetPointData()->AddArray(this->DualReturnMatching.GetPointer());
  }

  for (; firingBlock < HDL_FIRING_PER_PKT; ++firingBlock)
  {
    HDLFiringData* firingData = &(dataPacket->firingData[firingBlock]);
    // clang-format off
    int multiBlockLaserIdOffset =
        (firingData->blockIdentifier == BLOCK_0_TO_31)  ?  0 :(
        (firingData->blockIdentifier == BLOCK_32_TO_63) ? 32 :(
                                                           0));
    // clang-format on

    if (this->CurrentFrameState->hasChangedWithValue(*firingData))
    {
      this->SplitFrame();
      this->LastTimestamp = std::numeric_limits<unsigned int>::max();
    }

    // Skip this firing every PointSkip
    if (this->FiringsSkip == 0 || firingBlock % (this->FiringsSkip + 1) == 0)
    {
      this->ProcessFiring(firingData, multiBlockLaserIdOffset, firingBlock, azimuthDiff, timestamp,
        rawtime,
        this->HasDualReturn && dataPacket->isDualBlockOfDualPacket(this->IsHDL64Data, firingBlock),
        this->HasDualReturn, geotransform.GetPointer());
    }
  }
}

//-----------------------------------------------------------------------------
int vtkVelodyneHDLReader::ReadFrameInformation()
{
  vtkPacketFileReader reader;
  if (!reader.Open(this->Internal->FileName))
  {
    vtkErrorMacro("Failed to open packet file: " << this->Internal->FileName << endl
                                                 << reader.GetLastError());
    return 0;
  }

  const unsigned char* data = 0;
  unsigned int dataLength = 0;
  double timeSinceStart = 0;

  FramingState currentFrameState;
  unsigned int lastTimestamp = 0;

  std::vector<fpos_t> filePositions;
  std::vector<int> skips;
  unsigned long long numberOfFiringPackets = 0;
  unsigned long long lastnumberOfFiringPackets = 0;

  fpos_t lastFilePosition;
  reader.GetFilePosition(&lastFilePosition);

  bool IsHDL64Data = false;
  bool isEmptyFrame = true;
  while (reader.NextPacket(data, dataLength, timeSinceStart))
  {

    if (dataLength != 1206)
    {
      reader.GetFilePosition(&lastFilePosition);
      continue;
    }

    const HDLDataPacket* dataPacket = reinterpret_cast<const HDLDataPacket*>(data);
    numberOfFiringPackets++;

    //    unsigned int timeDiff = dataPacket->gpsTimestamp - lastTimestamp;
    //    if (timeDiff > 600 && lastTimestamp != 0)
    //      {
    //      printf("missed %d packets\n",  static_cast<int>(floor((timeDiff/553.0) + 0.5)));
    //      }

    for (int i = 0; i < HDL_FIRING_PER_PKT; ++i)
    {
      const HDLFiringData& firingData = dataPacket->firingData[i];

      IsHDL64Data |= (firingData.blockIdentifier == BLOCK_32_TO_63);

      // Test if all lasers had a positive distance
      if (this->Internal->IgnoreZeroDistances)
      {
        for (int laserID = 0; laserID < HDL_LASER_PER_FIRING; laserID++)
        {
          if (firingData.laserReturns[laserID].distance != 0)
            isEmptyFrame = false;
        }
      }
      else
      {
        isEmptyFrame = false;
      }

      if (currentFrameState.hasChangedWithValue(firingData))
      {
        // Add file position if the frame is not empty
        if (!isEmptyFrame || !this->Internal->IgnoreEmptyFrames)
        {
          filePositions.push_back(lastFilePosition);
          skips.push_back(i);
          PacketProcessingDebugMacro(
            << "\n\nEnd of frame #" << filePositions.size()
            << ". #packets: " << numberOfFiringPackets - lastnumberOfFiringPackets << "\n\n"
            << "RotationalPositions: ");
          lastnumberOfFiringPackets = numberOfFiringPackets;
        }
        this->UpdateProgress(0.0);
        // We start a new frame, reinitialize the boolean
        isEmptyFrame = true;
      }
      PacketProcessingDebugMacro(<< firingData.rotationalPosition << ", ");
    }

    // Accumulate HDL64 Status byte data
    if (IsHDL64Data && this->Internal->IsCorrectionFromLiveStream &&
      !this->Internal->IsCalibrated)
    {
      this->appendRollingDataAndTryCorrection(data);
    }
    lastTimestamp = dataPacket->gpsTimestamp;
    reader.GetFilePosition(&lastFilePosition);
  }

  if (IsHDL64Data && this->Internal->IsCorrectionFromLiveStream &&
    !this->Internal->IsCalibrated)
  {
    vtkGenericWarningMacro("Unable to load live calibration from pcap")
  }
  this->Internal->FilePositions = filePositions;
  this->Internal->Skips = skips;
  return this->GetNumberOfFrames();
}

//-----------------------------------------------------------------------------
bool vtkVelodyneHDLReader::updateReportedSensor(
  const unsigned char* data, unsigned int bytesReceived)
{
  if (HDLDataPacket::isValidPacket(data, bytesReceived))
  {
    const HDLDataPacket* dataPacket = reinterpret_cast<const HDLDataPacket*>(data);
    this->Internal->IsHDL64Data = dataPacket->isHDL64();
    this->Internal->ReportedSensor = dataPacket->getSensorType();
    this->Internal->ReportedFactoryField1 = dataPacket->factoryField1;
    this->Internal->ReportedFactoryField2 = dataPacket->factoryField2;
    return true;
  }
  return false;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetSelectedPointsWithDualReturn(double* data, int Npoints)
{
  this->Internal->SelectedDualReturn = vtkSmartPointer<vtkDoubleArray>::New();
  this->Internal->SelectedDualReturn->Allocate(60000);
  this->Internal->SelectedDualReturn->SetName("dualReturn_of_selectedPoints");

  for (unsigned int k = 0; k < Npoints; ++k)
  {
    this->Internal->SelectedDualReturn->InsertNextValue(data[k]);
  }
}

//-----------------------------------------------------------------------------
bool vtkVelodyneHDLReader::GetHasDualReturn()
{
  return this->Internal->HasDualReturn;
}

//-----------------------------------------------------------------------------
bool vtkVelodyneHDLReader::isReportedSensorAndCalibrationFileConsistent(bool shouldWarn)
{
  // Get the number of laser from sensor type
  int reportedSensorNumberLaser = num_laser(this->Internal->ReportedSensor);

  // compare the numbers of lasers
  if (reportedSensorNumberLaser != this->Internal->CalibrationReportedNumLasers)
  {
    if (shouldWarn && !this->Internal->AlreadyWarnAboutCalibration)
    {
      std::stringstream warningMessage;
      if (reportedSensorNumberLaser == 0)
      {
        warningMessage << "The data-packet from the sensor has an unrecognised "
                       << "factory byte (0x" << hex << Internal->ReportedSensor << dec << ")";
      }
      else
      {
        warningMessage << "The data-packet from the sensor has a factory byte "
                       << "(0x" << hex << Internal->ReportedSensor << dec << ") "
                       << "recognized as having " << reportedSensorNumberLaser << " lasers";
      }
      warningMessage << ", " << SOFTWARE_NAME << " will interpret data-packets and show points"
                     << " based on the XML calibration file only (currently: "
                     << this->Internal->CalibrationReportedNumLasers << " lasers).";
      vtkGenericWarningMacro(<< warningMessage.str());
      this->Internal->AlreadyWarnAboutCalibration = true;
    }
    return false;
  }

  return true;
}

#pragma pack(push, 1)
// Following struct are direct mapping from the manual
//      "Velodyne, Inc. ©2013  63‐HDL64ES3 REV G" Appendix E. Pages 31-42
struct HDLLaserCorrectionByte
{
  // This is the per laser 64-byte struct in the rolling data
  // It corresponds to 4 cycles of (9 HW status bytes + 7 calibration bytes)
  // WARNING data in packets are little-endian, which enables direct casting
  //  in short ONLY on little-endian machines (Intel & Co are fine)

  // Cycle n+0
  unsigned char hour_cycle0;
  unsigned char minutes_cycle0;
  unsigned char seconds_cycle0;
  unsigned char day_cycle0;
  unsigned char month_cycle0;
  unsigned char year_cycle0;
  unsigned char gpsSignalStatus_cycle0;
  unsigned char temperature_cycle0;
  unsigned char firmwareVersion_cycle0;
  unsigned char warningBit;          // 'U' in very first cycle (laser #0)
  unsigned char reserved1;           // 'N' in very first cycle (laser #0)
  unsigned char reserved2;           // 'I' in very first cycle (laser #0)
  unsigned char reserved3;           // 'T' in very first cycle (laser #0)
  unsigned char reserved4;           // '#' in very first cycle (laser #0)
  unsigned char upperBlockThreshold; // only in very first cycle (laser #0)
  unsigned char lowerBlockThreshold; // only in very first cycle (laser #0)

  // Cycle n+1
  unsigned char hour_cycle1;
  unsigned char minutes_cycle1;
  unsigned char seconds_cycle1;
  unsigned char day_cycle1;
  unsigned char month_cycle1;
  unsigned char year_cycle1;
  unsigned char gpsSignalStatus_cycle1;
  unsigned char temperature_cycle1;
  unsigned char firmwareVersion_cycle1;

  unsigned char channel;
  signed short verticalCorrection;    // This is in 100th of degree
  signed short rotationalCorrection;  // This is in 100th of degree
  signed short farDistanceCorrection; // This is in millimeter
  // Cycle n+2
  unsigned char hour_cycle2;
  unsigned char minutes_cycle2;
  unsigned char seconds_cycle2;
  unsigned char day_cycle2;
  unsigned char month_cycle2;
  unsigned char year_cycle2;
  unsigned char gpsSignalStatus_cycle2;
  unsigned char temperature_cycle2;
  unsigned char firmwareVersion_cycle2;

  signed short distanceCorrectionX;
  signed short distanceCorrectionV;
  signed short verticalOffset;

  unsigned char horizontalOffsetByte1;
  // Cycle n+3
  unsigned char hour_cycle3;
  unsigned char minutes_cycle3;
  unsigned char seconds_cycle3;
  unsigned char day_cycle3;
  unsigned char month_cycle3;
  unsigned char year_cycle3;
  unsigned char gpsSignalStatus_cycle3;
  unsigned char temperature_cycle3;
  unsigned char firmwareVersion_cycle3;

  unsigned char horizontalOffsetByte2;

  signed short focalDistance;
  signed short focalSlope;

  unsigned char minIntensity;
  unsigned char maxIntensity;
};

struct last4cyclesByte
{
  // Cycle n+0
  unsigned char hour_cycle0;
  unsigned char minutes_cycle0;
  unsigned char seconds_cycle0;
  unsigned char day_cycle0;
  unsigned char month_cycle0;
  unsigned char year_cycle0;
  unsigned char gpsSignalStatus_cycle0;
  unsigned char temperature_cycle0;
  unsigned char firmwareVersion_cycle0;

  unsigned char calibration_year;
  unsigned char calibration_month;
  unsigned char calibration_day;
  unsigned char calibration_hour;
  unsigned char calibration_minutes;
  unsigned char calibration_seconds;
  unsigned char humidity;
  // Cycle n+1
  unsigned char hour_cycle1;
  unsigned char minutes_cycle1;
  unsigned char seconds_cycle1;
  unsigned char day_cycle1;
  unsigned char month_cycle1;
  unsigned char year_cycle1;
  unsigned char gpsSignalStatus_cycle1;
  unsigned char temperature_cycle1;
  unsigned char firmwareVersion_cycle1;

  signed short motorRPM;
  unsigned short fovStartAngle; // in 100th of degree
  unsigned short fovEndAngle;   // in 100th of degree
  unsigned char realLifeTimeByte1;
  // Cycle n+2
  unsigned char hour_cycle2;
  unsigned char minutes_cycle2;
  unsigned char seconds_cycle2;
  unsigned char day_cycle2;
  unsigned char month_cycle2;
  unsigned char year_cycle2;
  unsigned char gpsSignalStatus_cycle2;
  unsigned char temperature_cycle2;
  unsigned char firmwareVersion_cycle2;

  unsigned char realLifeTimeByte2;

  unsigned char sourceIPByte1;
  unsigned char sourceIPByte2;
  unsigned char sourceIPByte3;
  unsigned char sourceIPByte4;

  unsigned char destinationIPByte1;
  unsigned char destinationIPByte2;
  // Cycle n+3
  unsigned char hour_cycle3;
  unsigned char minutes_cycle3;
  unsigned char seconds_cycle3;
  unsigned char day_cycle3;
  unsigned char month_cycle3;
  unsigned char year_cycle3;
  unsigned char gpsSignalStatus_cycle3;
  unsigned char temperature_cycle3;
  unsigned char firmwareVersion_cycle3;

  unsigned char destinationIPByte3;
  unsigned char destinationIPByte4;
  unsigned char multipleReturnStatus; // 0= Strongest, 1= Last, 2= Both
  unsigned char reserved3;
  unsigned char powerLevelStatus;
  unsigned short calibrationDataCRC;
};

#pragma pack(pop)
//
bool vtkVelodyneHDLReader::vtkInternal::HDL64LoadCorrectionsFromStreamData()
{
  std::vector<unsigned char> data;
  if (!this->rollingCalibrationData->getAlignedRollingData(data))
  {
    return false;
  }
  // the rollingCalibrationData considers the marker to be "#" in reserved4
  const int idxDSRDataFromMarker =
    static_cast<int>(-reinterpret_cast<unsigned long>(&((HDLLaserCorrectionByte*)0)->reserved4));
  const int HDL64_RollingData_NumLaser = 64;
  for (int dsr = 0; dsr < HDL64_RollingData_NumLaser; ++dsr)
  {
    const HDLLaserCorrectionByte* correctionStream = reinterpret_cast<const HDLLaserCorrectionByte*>
      // The 64 here is the length of the 4 16-byte cycle
      //    containing one dsr information
      (&data[idxDSRDataFromMarker + 64 * dsr]);
    if (correctionStream->channel != dsr)
    {
      return false;
    }
    HDLLaserCorrection& vvCorrection = laser_corrections_[correctionStream->channel];
    vvCorrection.verticalCorrection = correctionStream->verticalCorrection / 100.0;
    vvCorrection.rotationalCorrection = correctionStream->rotationalCorrection / 100.0;
    vvCorrection.distanceCorrection = correctionStream->farDistanceCorrection / 1000.0;

    vvCorrection.distanceCorrectionX = correctionStream->distanceCorrectionX / 1000.0;
    vvCorrection.distanceCorrectionY = correctionStream->distanceCorrectionV / 1000.0;
    vvCorrection.verticalOffsetCorrection = correctionStream->verticalOffset / 1000.0;
    // The following manipulation is needed because of the two byte for this
    //  parameter are not side-by-side
    vvCorrection.horizontalOffsetCorrection =
      this->rollingCalibrationData->fromTwoLittleEndianBytes<signed short>(
        correctionStream->horizontalOffsetByte1, correctionStream->horizontalOffsetByte2) /
      1000.0;
    vvCorrection.focalDistance = correctionStream->focalDistance / 1000.0;
    vvCorrection.focalSlope = correctionStream->focalSlope / 1000.0;
    vvCorrection.closeSlope = correctionStream->focalSlope / 1000.0;
    vvCorrection.minIntensity = correctionStream->minIntensity;
    vvCorrection.maxIntensity = correctionStream->maxIntensity;
  }

  // Get the last cycle of live correction file
  const last4cyclesByte* lastCycle = reinterpret_cast<const last4cyclesByte*>(
    &data[idxDSRDataFromMarker + 64 * HDL64_RollingData_NumLaser]);
  this->SensorPowerMode = lastCycle->powerLevelStatus;
  this->ReportedSensorReturnMode = ((lastCycle->multipleReturnStatus == 0)
      ? STRONGEST_RETURN
      : ((lastCycle->multipleReturnStatus == 1) ? LAST_RETURN : DUAL_RETURN));

  this->CalibrationReportedNumLasers = HDL64_RollingData_NumLaser;
  PrecomputeCorrectionCosSin();
  this->IsCalibrated = true;
  return true;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::appendRollingDataAndTryCorrection(const unsigned char* data)
{
  const HDLDataPacket* dataPacket = reinterpret_cast<const HDLDataPacket*>(data);
  this->Internal->rollingCalibrationData->appendData(
    dataPacket->gpsTimestamp, dataPacket->factoryField1, dataPacket->factoryField2);
  this->Internal->HDL64LoadCorrectionsFromStreamData();
}

//-----------------------------------------------------------------------------
bool vtkVelodyneHDLReader::getIsHDL64Data()
{
  return this->Internal->IsHDL64Data;
}

//-----------------------------------------------------------------------------
bool vtkVelodyneHDLReader::IsIntensityCorrectedBySensor()
{
  return this->Internal->SensorPowerMode == CorrectionOn;
}

//-----------------------------------------------------------------------------
const bool& vtkVelodyneHDLReader::GetWantIntensityCorrection()
{
  return this->Internal->WantIntensityCorrection;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLReader::SetIntensitiesCorrected(const bool& state)
{

  if (state != this->Internal->WantIntensityCorrection)
  {
    this->Internal->WantIntensityCorrection = state;
    this->Modified();
  }
}
