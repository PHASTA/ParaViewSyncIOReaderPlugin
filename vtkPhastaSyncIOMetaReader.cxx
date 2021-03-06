/*=========================================================================

  Program:   Visualization Toolkit
  Module:    $RCSfile: vtkPhastaSyncIOMetaReader.cxx,v $

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkPhastaSyncIOMetaReader.h"

#include "vtkCellData.h"
#include "vtkFieldData.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkMultiBlockDataSet.h"
#include "vtkMultiPieceDataSet.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkPVXMLElement.h"
#include "vtkPVXMLParser.h"
#include "vtkPhastaSyncIOReader.h"
#include "vtkSmartPointer.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkUnstructuredGrid.h"

#include <vtksys/SystemTools.hxx>

#include <map>
#include <string>
#include <sstream>

int NUM_PIECES;
int NUM_FILES;
int TIME_STEP;
char * FILE_PATH;
int PART_ID;
int FILE_ID;

double opentime_total = 0.0;

/*
 * Modified part is dealing with new phasta data format
 * SyncIO and rbIO library, contact liun2@cs.rpi.edu
 *
 *               ------ Ning Liu
 *               ------ Sept. 2010
 */

struct vtkPhastaSyncIOMetaReaderInternal
{
  struct TimeStepInfo
  {
    int GeomIndex;
    int FieldIndex;
    double TimeValue;

    TimeStepInfo() : GeomIndex(-1), FieldIndex(-1), TimeValue(0.0)
      {
      }
  };

  typedef std::map<int, TimeStepInfo> TimeStepInfoMapType;
  TimeStepInfoMapType TimeStepInfoMap;
  typedef std::map<int, vtkSmartPointer<vtkUnstructuredGrid> >
  CachedGridsMapType;
  CachedGridsMapType CachedGrids;
};
//----------------------------------------------------------------------------
vtkStandardNewMacro(vtkPhastaSyncIOMetaReader);
//----------------------------------------------------------------------------
vtkPhastaSyncIOMetaReader::vtkPhastaSyncIOMetaReader()
{
  //this->DebugOn(); // comment out this line when in production
  this->FileName = 0;

  this->TimeStepIndex = 0;
  this->ActualTimeStep = 0;

  this->Reader = vtkPhastaSyncIOReader::New();

  this->SetNumberOfInputPorts(0);

  this->Parser = 0;

  this->Internal = new vtkPhastaSyncIOMetaReaderInternal;

  this->TimeStepRange[0] = 0;
  this->TimeStepRange[1] = 0;
}

//----------------------------------------------------------------------------
vtkPhastaSyncIOMetaReader::~vtkPhastaSyncIOMetaReader()
{
  this->Reader->Delete();
  this->SetFileName(0);

  if (this->Parser)
    {
    this->Parser->Delete();
    }

  delete this->Internal;
}

//----------------------------------------------------------------------------
int vtkPhastaSyncIOMetaReader::RequestData(vtkInformation*,
                                  vtkInformationVector**,
                                  vtkInformationVector* outputVector)
{
  vtkDebugMacro("Entering PP RequestData()\n");
  // get the data object
  vtkInformation *outInfo =
    outputVector->GetInformationObject(0);

  this->ActualTimeStep = this->TimeStepIndex;

  int tsLength =
    outInfo->Length(vtkStreamingDemandDrivenPipeline::TIME_STEPS());
  double* steps =
    outInfo->Get(vtkStreamingDemandDrivenPipeline::TIME_STEPS());

  // Check if a particular time was requested.
  if(outInfo->Has(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEP()))
    {
    // Get the requested time step. We only supprt requests of a single time
    // step in this reader right now
    double requestedTimeSteps =
      outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEP());
    double timeValue = requestedTimeSteps;

    // find the first time value larger than requested time value
    // this logic could be improved
    int cnt = 0;
    while (cnt < tsLength-1 && steps[cnt] < timeValue)
      {
      cnt++;
      }
    this->ActualTimeStep = cnt;
    }

  if (this->ActualTimeStep > this->TimeStepRange[1])
    {
    vtkErrorMacro("Timestep index too large.");
    return 0;
    }

  // get the current piece being requested
  int piece =
    outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_PIECE_NUMBER());

  int numProcPieces =
    outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_NUMBER_OF_PIECES());

  // this is actually # of proc
  vtkDebugMacro(<<"numProcPieces (i.e. # of proc) = "<< numProcPieces);

  if (!this->Parser)
    {
    vtkErrorMacro("No parser was created. Cannot read file");
    return 0;
    }

  vtkPVXMLElement* rootElement = this->Parser->GetRootElement();

  int numPieces;
  int numFiles, timeStep;
  if (!rootElement->GetScalarAttribute("number_of_pieces", &numPieces))
    {
    numPieces = 1;
    }

  if (!rootElement->GetScalarAttribute("number_of_files", &numFiles))
    {
    numFiles = 1;
    }

  NUM_PIECES=numPieces;
  NUM_FILES=numFiles;

  vtkDebugMacro(<<"NEW PHT Parameter: number_of_files = "<< numFiles );

  vtkMultiBlockDataSet *output = vtkMultiBlockDataSet::SafeDownCast(
    outInfo->Get(vtkDataObject::DATA_OBJECT()));
  output->SetNumberOfBlocks(1);
  vtkMultiPieceDataSet* MultiPieceDataSet = vtkMultiPieceDataSet::New();
  // the following line was deleted in Ning's version
  MultiPieceDataSet->SetNumberOfPieces(numPieces);
  output->SetBlock(0, MultiPieceDataSet);
  MultiPieceDataSet->Delete();

  const char* geometryPattern = 0;
  int geomHasPiece = 0;
  int geomHasTime = 0;
  const char* fieldPattern = 0;
  int fieldHasPiece = 0;
  int fieldHasTime = 0;

  unsigned int numElements = rootElement->GetNumberOfNestedElements();
  for (unsigned int i=0; i<numElements; i++)
    {
    vtkPVXMLElement* nested = rootElement->GetNestedElement(i);

    if (strcmp("GeometryFileNamePattern", nested->GetName()) == 0)
      {
      geometryPattern = nested->GetAttribute("pattern");
      if (!nested->GetScalarAttribute("has_piece_entry", &geomHasPiece))
        {
        geomHasPiece = 0;
        }
      if (!nested->GetScalarAttribute("has_time_entry", &geomHasTime))
        {
        geomHasTime = 0;
        }
      }

    if (strcmp("FieldFileNamePattern", nested->GetName()) == 0)
      {
      fieldPattern = nested->GetAttribute("pattern");
      if (!nested->GetScalarAttribute("has_piece_entry", &fieldHasPiece))
        {
        fieldHasPiece = 0;
        }
      if (!nested->GetScalarAttribute("has_time_entry", &fieldHasTime))
        {
        fieldHasTime = 0;
        }
      }
    }

  if (!geometryPattern)
    {
    vtkErrorMacro("No geometry pattern was specified. Cannot load file");
    return 0;
    }

  if (!fieldPattern)
    {
    vtkErrorMacro("No field pattern was specified. Cannot load file");
    return 0;
    }

  char* geom_name = new char [ strlen(geometryPattern) + 60 ];
  char* field_name = new char [ strlen(fieldPattern) + 60 ];

  //////////////////////
  int numPiecesPerFile = numPieces/numFiles;
  //////////////////////

  // now loop over all of the files that I should load
  for(int loadingPiece=piece;loadingPiece<numPieces;loadingPiece+=numProcPieces)
    {
      TIME_STEP=this->Internal->TimeStepInfoMap[this->ActualTimeStep].FieldIndex;
      FILE_ID = int(loadingPiece/numPiecesPerFile)+1; // this will be passed to PhastaReader by extern...
      PART_ID = loadingPiece+1;

      vtkDebugMacro(<<"PP In loop, piece="<< piece <<", loadingPiece+1="<< loadingPiece +1 << ", numPieces="<<numPieces<<", FILE_ID=" << FILE_ID<<", numProcPieces=" << numProcPieces);

    if (geomHasTime && geomHasPiece)
      {
      sprintf(geom_name,
              geometryPattern,
              this->Internal->TimeStepInfoMap[this->ActualTimeStep].GeomIndex,
              FILE_ID);
      }
    else if (geomHasPiece)
      {
        sprintf(geom_name, geometryPattern, FILE_ID);
      }
    else if (geomHasTime)
      {
      sprintf(geom_name,
              geometryPattern,
              this->Internal->TimeStepInfoMap[this->ActualTimeStep].GeomIndex);
      }
    else
      {
      strcpy(geom_name, geometryPattern);
      }

    if (fieldHasTime && fieldHasPiece)
      {
      sprintf(field_name,
              fieldPattern,
              this->Internal->TimeStepInfoMap[this->ActualTimeStep].FieldIndex,
  //          FILE_ID); // don't use file id, otherwise dup geom and field file id will make PhastaReader not update -- jingfu
              loadingPiece+1);
      }
    else if (fieldHasPiece)
      {
      sprintf(field_name, fieldPattern, loadingPiece+1);
  //FILE_ID);
      }
    else if (fieldHasTime)
      {
      sprintf(field_name,
              fieldPattern,
              this->Internal->TimeStepInfoMap[this->ActualTimeStep].FieldIndex);
      }
    else
      {
      strcpy(geom_name, fieldPattern);
      }

    std::ostringstream geomFName;
    std::string gpath = vtksys::SystemTools::GetFilenamePath(geom_name);
    if (gpath.empty() || !vtksys::SystemTools::FileIsFullPath(gpath.c_str()))
      {
      std::string path = vtksys::SystemTools::GetFilenamePath(this->FileName);
      if (!path.empty())
        {
        geomFName << path.c_str() << "/";
        }
      }
    geomFName << geom_name << ends;
    this->Reader->SetGeometryFileName(geomFName.str().c_str());

    std::ostringstream fieldFName;
    // try to strip out the path of file, if it's a full path file name
    std::string fpath = vtksys::SystemTools::GetFilenamePath(field_name);

    ///////////////////////////////////////////
    FILE_PATH = new char[fpath.size()+1];
    strcpy(FILE_PATH,fpath.c_str());
    ///////////////////////////////////////////

    if (fpath.empty() || !vtksys::SystemTools::FileIsFullPath(fpath.c_str()))
      {
      std::string path = vtksys::SystemTools::GetFilenamePath(this->FileName); // FileName is the .pht file
      if (!path.empty())
        {
          /////////////////////////////////////////
          delete [] FILE_PATH;
          FILE_PATH = new char[path.size()+1];
          strcpy(FILE_PATH,path.c_str());
          /////////////////////////////////////////
          fieldFName << path.c_str() << "/";
          //std::cout << "something might be wrong here, string path=" << path << ", fieldFName=" << fieldFName<< std::endl;
        }
      }
    fieldFName << field_name << ends;
    this->Reader->SetFieldFileName(fieldFName.str().c_str());

    vtkPhastaSyncIOMetaReaderInternal::CachedGridsMapType::iterator CachedCopy =
      this->Internal->CachedGrids.find(loadingPiece);

    // the following "if" was commented out in previous new version (tweaked by Ning)
    // if there is a cached copy, use that
    /*
    if(CachedCopy != this->Internal->CachedGrids.end())
      {
      this->Reader->SetCachedGrid(CachedCopy->second);
      printf("should use cached copy but can't compile\n");
      }
      */

    // In order to register etc, Reader need a new executative in every
    // update call, otherwise it doesn't do anything
    this->Reader->Update();
    // the following "if" was commented out in previous new version (tweaked by Ning)

    /*
    if(CachedCopy == this->Internal->CachedGrids.end())
      {
      vtkSmartPointer<vtkUnstructuredGrid> cached =
        vtkSmartPointer<vtkUnstructuredGrid>::New();
      cached->ShallowCopy(this->Reader->GetOutput());
      cached->GetPointData()->Initialize();
      cached->GetCellData()->Initialize();
      cached->GetFieldData()->Initialize();
      this->Internal->CachedGrids[loadingPiece] = cached;
      }
      */

    vtkSmartPointer<vtkUnstructuredGrid> copy =
      vtkSmartPointer<vtkUnstructuredGrid>::New();
    copy->ShallowCopy(this->Reader->GetOutput());
    MultiPieceDataSet->SetPiece(loadingPiece, copy);
    //MultiPieceDataSet->SetPiece(MultiPieceDataSet->GetNumberOfPieces(),copy); // Ning's version
    }

  delete [] FILE_PATH;
  delete [] geom_name;
  delete [] field_name;

  if (steps)
    {
    output->GetInformation()->Set(vtkDataObject::DATA_TIME_STEP(),
                                  steps[this->ActualTimeStep]);
    }

  vtkDebugMacro("End of PP RequestData()\n, total open time is " << opentime_total);
  // if it's not too many printf, print it out
  if( numProcPieces < 16) printf("total open time for sync-io is %lf (nf=%d, np=%d)\n", opentime_total, numFiles, numProcPieces);

  return 1;
}

//----------------------------------------------------------------------------
int vtkPhastaSyncIOMetaReader::RequestInformation(vtkInformation*,
                                       vtkInformationVector**,
                                       vtkInformationVector* outputVector)
{
  vtkDebugMacro(<<"In PP requestInformation() -- nothing modified in this func\n");
  this->Internal->TimeStepInfoMap.clear();
  this->Reader->ClearFieldInfo();

  vtkInformation *outInfo =
    outputVector->GetInformationObject(0);

  if (!this->FileName)
    {
    vtkErrorMacro("FileName has to be specified.");
    return 0;
    }

  if (this->Parser)
    {
    this->Parser->Delete();
    this->Parser = 0;
    }

  vtkSmartPointer<vtkPVXMLParser> parser =
    vtkSmartPointer<vtkPVXMLParser>::New();

  parser->SetFileName(this->FileName);
  if (!parser->Parse())
    {
    return 0;
    }

  vtkPVXMLElement* rootElement = parser->GetRootElement();
  if (!rootElement)
    {
    vtkErrorMacro("Cannot parse file.");
    return 0;
    }

  if (strcmp(rootElement->GetName(), "PhastaMetaFile") != 0)
    {
    vtkErrorMacro("This is not a phasta metafile.");
    return 0;
    }

  this->Parser = parser;
  parser->Register(this);

  int numTimeSteps=1;
  int hasTimeValues = 0;

  unsigned int numElements = rootElement->GetNumberOfNestedElements();
  for (unsigned int i=0; i<numElements; i++)
    {
    vtkPVXMLElement* nested = rootElement->GetNestedElement(i);
    if (strcmp("TimeSteps", nested->GetName()) == 0)
      {
      if (!nested->GetScalarAttribute("number_of_steps", &numTimeSteps))
        {
        numTimeSteps = 1;
        }
      int autoGen;
      int indexIncr;
      int startIndex;
      if (!nested->GetScalarAttribute("auto_generate_indices", &autoGen))
        {
        autoGen = 0;
        }
      if (!nested->GetScalarAttribute("increment_index_by", &indexIncr))
        {
        indexIncr = 1;
        }
      if (!nested->GetScalarAttribute("start_index", &startIndex))
        {
        startIndex = 0;
        }
      double startValue = 0.;
      double valueIncr = 1.*indexIncr;
      if (nested->GetScalarAttribute("start_value", &startValue))
        {
        hasTimeValues = 1;
        }
      if (nested->GetScalarAttribute("increment_value_by", &valueIncr))
        {
        hasTimeValues = 1;
        }
      if (autoGen)
        {
        for (int j=0; j<numTimeSteps; j++)
          {
          vtkPhastaSyncIOMetaReaderInternal::TimeStepInfo& info =
            this->Internal->TimeStepInfoMap[j];
          info.GeomIndex = startIndex;
          info.FieldIndex = startIndex;
          info.TimeValue = startValue;
          startIndex += indexIncr;
          startValue += valueIncr;
          }
        }

      unsigned int numElements2 = nested->GetNumberOfNestedElements();
      for (unsigned int j=0; j<numElements2; j++)
        {
        vtkPVXMLElement* nested2 = nested->GetNestedElement(j);
        if (strcmp("TimeStep", nested2->GetName()) == 0)
          {
          int index;
          if (nested2->GetScalarAttribute("index", &index))
            {
            if ( (index+1) > numTimeSteps )
              {
              numTimeSteps = index+1;
              }
            vtkPhastaSyncIOMetaReaderInternal::TimeStepInfo& info =
              this->Internal->TimeStepInfoMap[index];
            int gIdx;
            if (nested2->GetScalarAttribute("geometry_index",
                                             &gIdx))
              {
              info.GeomIndex = gIdx;
              }
            int fIdx;
            if (nested2->GetScalarAttribute("field_index",
                                             &fIdx))
              {
              info.FieldIndex = fIdx;
              }
            double val;
            if (nested2->GetScalarAttribute("value",
                                            &val))
              {
              info.TimeValue = val;
              hasTimeValues = 1;
              }
            }
          }
        }
      break;
      }
    }

  int numberOfFields=0, numberOfFields2=0;
  for (unsigned int i=0; i<numElements; i++)
    {
    vtkPVXMLElement* nested = rootElement->GetNestedElement(i);
    if (strcmp("Fields", nested->GetName()) == 0)
      {
      if (!nested->GetScalarAttribute("number_of_fields", &numberOfFields))
        {
        numberOfFields = 1;
        }

      numberOfFields2 = 0;
      unsigned int numElements2 = nested->GetNumberOfNestedElements();
      for (unsigned int j=0; j<numElements2; j++)
        {
        vtkPVXMLElement* nested2 = nested->GetNestedElement(j);
        if (strcmp("Field", nested2->GetName()) == 0)
          {
          numberOfFields2++;
          std::string paraviewFieldTagStr, dataTypeStr;
          const char* paraviewFieldTag = 0;
          paraviewFieldTag = nested2->GetAttribute("paraview_field_tag");
          if (!paraviewFieldTag)
            {
            std::ostringstream paraviewFieldTagStrStream;
            paraviewFieldTagStrStream << "Field " << numberOfFields2 << ends;
            paraviewFieldTagStr = paraviewFieldTagStrStream.str();
            paraviewFieldTag = paraviewFieldTagStr.c_str();
            }
          const char* phastaFieldTag = 0;
          phastaFieldTag = nested2->GetAttribute("phasta_field_tag");
          if (!phastaFieldTag)
            {
            vtkErrorMacro("No phasta field tag was specified");
            return 0;
            }
          int index; // 0 as default (for safety)
          if (!nested2->GetScalarAttribute("start_index_in_phasta_array", &index))
            {
            index = 0;
            }
          int numOfComps; // 1 as default (for safety)
          if (!nested2->GetScalarAttribute("number_of_components", &numOfComps))
            {
            numOfComps = 1;
            }
          int dataDependency; // nodal as default
          if (!nested2->GetScalarAttribute("data_dependency", &dataDependency))
            {
            dataDependency = 0;
            }
          const char* dataType = 0;
          dataType = nested2->GetAttribute("data_type");
          if (!dataType) // "double" as default
            {
            dataTypeStr = "double";
            dataType = dataTypeStr.c_str();
            }

          this->Reader->SetFieldInfo(paraviewFieldTag,phastaFieldTag,index,numOfComps,dataDependency,dataType);
          }
        }

      if (numberOfFields<numberOfFields2)
        {
        numberOfFields = numberOfFields2;
        }

      break;
      }
    }

  if (!numberOfFields2) // by default take "solution" with only flow variables
    {
    numberOfFields = 3;
    this->Reader->SetFieldInfo("pressure","solution",0,1,0,"double");
    this->Reader->SetFieldInfo("velocity","solution",1,3,0,"double");
    this->Reader->SetFieldInfo("temperature","solution",4,1,0,"double");
    }

  int tidx;
  // Make sure all indices are there
  for (tidx=1; tidx<numTimeSteps; tidx++)
    {
    vtkPhastaSyncIOMetaReaderInternal::TimeStepInfoMapType::iterator iter =
      this->Internal->TimeStepInfoMap.find(tidx);
    if (iter == this->Internal->TimeStepInfoMap.end())
      {
      vtkErrorMacro("Missing timestep, index=" << tidx);
      return 0;
      }
    }

  if (hasTimeValues)
    {
    double* timeSteps = new double[numTimeSteps];
    for (tidx=0; tidx<numTimeSteps; tidx++)
      {
      timeSteps[tidx] = this->Internal->TimeStepInfoMap[tidx].TimeValue;
      }
    outInfo->Set(vtkStreamingDemandDrivenPipeline::TIME_STEPS(),
                 timeSteps,
                 numTimeSteps);
    double timeRange[2];
    timeRange[0] = timeSteps[0];
    timeRange[1] = timeSteps[numTimeSteps-1];
    outInfo->Set(vtkStreamingDemandDrivenPipeline::TIME_RANGE(),
                     timeRange, 2);
    delete[] timeSteps;
    }

  this->TimeStepRange[0] = 0;
  this->TimeStepRange[1] = numTimeSteps-1;

  vtkInformation* info = outputVector->GetInformationObject(0);
  info->Set(
    CAN_HANDLE_PIECE_REQUEST(), 1);

  return 1;
}

//-----------------------------------------------------------------------------
int vtkPhastaSyncIOMetaReader::CanReadFile(const char *filename)
{
  vtkSmartPointer<vtkPVXMLParser> parser
    = vtkSmartPointer<vtkPVXMLParser>::New();
  parser->SuppressErrorMessagesOn();
  parser->SetFileName(filename);

  // Make sure we can parse the XML metafile.
  if (!parser->Parse()) return 0;

  // Make sure the XML file has a root element and it is of the right tag.
  vtkPVXMLElement *rootElement = parser->GetRootElement();
  if (!rootElement) return 0;
  if (strcmp(rootElement->GetName(), "PhastaMetaFile") != 0) return 0;

  // The file clearly is supposed to be a Phasta file.
  return 1;
}

//----------------------------------------------------------------------------
void vtkPhastaSyncIOMetaReader::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);

  os << indent << "FileName: "
     << (this->FileName?this->FileName:"(none)")
     << endl;
  os << indent << "TimeStepIndex: " << this->TimeStepIndex << endl;
  os << indent << "TimeStepRange: "
     << this->TimeStepRange[0] << " " << this->TimeStepRange[1]
     << endl;
}

