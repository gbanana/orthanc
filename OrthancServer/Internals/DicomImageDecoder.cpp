/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2014 Medical Physics Department, CHU of Liege,
 * Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * In addition, as a special exception, the copyright holders of this
 * program give permission to link the code of its release with the
 * OpenSSL project's "OpenSSL" library (or with modified versions of it
 * that use the same license as the "OpenSSL" library), and distribute
 * the linked executables. You must obey the GNU General Public License
 * in all respects for all of the code used other than "OpenSSL". If you
 * modify file(s) with this exception, you may extend this exception to
 * your version of the file(s), but you are not obligated to do so. If
 * you do not wish to do so, delete this exception statement from your
 * version. If you delete this exception statement from all source files
 * in the program, then also delete it here.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 **/


#include "../PrecompiledHeadersServer.h"
#include "DicomImageDecoder.h"


/*=========================================================================

  This file is based on portions of the following project
  (cf. function "DecodePsmctRle1()"):

  Program: GDCM (Grassroots DICOM). A DICOM library
  Module:  http://gdcm.sourceforge.net/Copyright.html

Copyright (c) 2006-2011 Mathieu Malaterre
Copyright (c) 1993-2005 CREATIS
(CREATIS = Centre de Recherche et d'Applications en Traitement de l'Image)
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither name of Mathieu Malaterre, or CREATIS, nor the names of any
   contributors (CNRS, INSERM, UCB, Universite Lyon I), may be used to
   endorse or promote products derived from this software without specific
   prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

=========================================================================*/



#include "../../Core/OrthancException.h"
#include "../../Core/DicomFormat/DicomIntegerPixelAccessor.h"
#include "../ToDcmtkBridge.h"
#include "../FromDcmtkBridge.h"

#include <glog/logging.h>

#include <boost/lexical_cast.hpp>

#if ORTHANC_JPEG_LOSSLESS_ENABLED == 1
#include <dcmtk/dcmjpls/djcodecd.h>
#include <dcmtk/dcmjpls/djcparam.h>
#include <dcmtk/dcmjpeg/djrplol.h>
#endif


namespace Orthanc
{
  static const DicomTag DICOM_TAG_CONTENT(0x07a1, 0x100a);
  static const DicomTag DICOM_TAG_COMPRESSION_TYPE(0x07a1, 0x1011);

  bool DicomImageDecoder::IsPsmctRle1(DcmDataset& dataset)
  {
    DcmElement* e;
    char* c;

    // Check whether the DICOM instance contains an image encoded with
    // the PMSCT_RLE1 scheme.
    if (!dataset.findAndGetElement(ToDcmtkBridge::Convert(DICOM_TAG_COMPRESSION_TYPE), e).good() ||
        e == NULL ||
        !e->isaString() ||
        !e->getString(c).good() ||
        c == NULL ||
        strcmp("PMSCT_RLE1", c))
    {
      return false;
    }
    else
    {
      return true;
    }
  }


  bool DicomImageDecoder::DecodePsmctRle1(std::string& output,
                                          DcmDataset& dataset)
  {
    // Check whether the DICOM instance contains an image encoded with
    // the PMSCT_RLE1 scheme.
    if (!IsPsmctRle1(dataset))
    {
      return false;
    }

    // OK, this is a custom RLE encoding from Philips. Get the pixel
    // data from the appropriate private DICOM tag.
    Uint8* pixData = NULL;
    DcmElement* e;
    if (!dataset.findAndGetElement(ToDcmtkBridge::Convert(DICOM_TAG_CONTENT), e).good() ||
        e == NULL ||
        e->getUint8Array(pixData) != EC_Normal)
    {
      return false;
    }    

    // The "unsigned" below IS VERY IMPORTANT
    const uint8_t* inbuffer = reinterpret_cast<const uint8_t*>(pixData);
    const size_t length = e->getLength();

    /**
     * The code below is an adaptation of a sample code for GDCM by
     * Mathieu Malaterre (under a BSD license).
     * http://gdcm.sourceforge.net/html/rle2img_8cxx-example.html
     **/

    // RLE pass
    std::vector<uint8_t> temp;
    temp.reserve(length);
    for (size_t i = 0; i < length; i++)
    {
      if (inbuffer[i] == 0xa5)
      {
        temp.push_back(inbuffer[i+2]);
        for (uint8_t repeat = inbuffer[i + 1]; repeat != 0; repeat--)
        {
          temp.push_back(inbuffer[i+2]);
        }
        i += 2;
      }
      else
      {
        temp.push_back(inbuffer[i]);
      }
    }

    // Delta encoding pass
    uint16_t delta = 0;
    output.clear();
    output.reserve(temp.size());
    for (size_t i = 0; i < temp.size(); i++)
    {
      uint16_t value;

      if (temp[i] == 0x5a)
      {
        uint16_t v1 = temp[i + 1];
        uint16_t v2 = temp[i + 2];
        value = (v2 << 8) + v1;
        i += 2;
      }
      else
      {
        value = delta + (int8_t) temp[i];
      }

      output.push_back(value & 0xff);
      output.push_back(value >> 8);
      delta = value;
    }

    if (output.size() % 2)
    {
      output.resize(output.size() - 1);
    }

    return true;
  }


  void DicomImageDecoder::SetupImageBuffer(ImageBuffer& target,
                                           DcmDataset& dataset)
  {
    OFString value;

    if (!dataset.findAndGetOFString(ToDcmtkBridge::Convert(DICOM_TAG_COLUMNS), value).good())
    {
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    unsigned int width = boost::lexical_cast<unsigned int>(value.c_str());

    if (!dataset.findAndGetOFString(ToDcmtkBridge::Convert(DICOM_TAG_ROWS), value).good())
    {
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    unsigned int height = boost::lexical_cast<unsigned int>(value.c_str());

    if (!dataset.findAndGetOFString(ToDcmtkBridge::Convert(DICOM_TAG_BITS_STORED), value).good())
    {
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    unsigned int bitsStored = boost::lexical_cast<unsigned int>(value.c_str());

    if (!dataset.findAndGetOFString(ToDcmtkBridge::Convert(DICOM_TAG_PIXEL_REPRESENTATION), value).good())
    {
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    bool isSigned = (boost::lexical_cast<unsigned int>(value.c_str()) != 0);

    unsigned int samplesPerPixel = 1; // By default
    if (dataset.findAndGetOFString(ToDcmtkBridge::Convert(DICOM_TAG_SAMPLES_PER_PIXEL), value).good())
    {
      samplesPerPixel = boost::lexical_cast<unsigned int>(value.c_str());
    }

    target.SetHeight(height);
    target.SetWidth(width);

    if (bitsStored == 8 && samplesPerPixel == 1 && !isSigned)
    {
      target.SetFormat(PixelFormat_Grayscale8);
    }
    else if (bitsStored == 8 && samplesPerPixel == 3 && !isSigned)
    {
      target.SetFormat(PixelFormat_RGB24);
    }
    else if (bitsStored == 16 && samplesPerPixel == 1 && !isSigned)
    {
      target.SetFormat(PixelFormat_Grayscale16);
    }
    else if (bitsStored == 16 && samplesPerPixel == 1 && isSigned)
    {
      target.SetFormat(PixelFormat_SignedGrayscale16);
    }
    else
    {
      throw OrthancException(ErrorCode_NotImplemented);
    }
  }


  bool DicomImageDecoder::IsJpegLossless(const DcmDataset& dataset)
  {
    return (dataset.getOriginalXfer() == EXS_JPEGLSLossless ||
            dataset.getOriginalXfer() == EXS_JPEGLSLossy);
  }


  bool DicomImageDecoder::IsUncompressedImage(const DcmDataset& dataset)
  {
    return (dataset.getOriginalXfer() == EXS_Unknown ||
            dataset.getOriginalXfer() == EXS_LittleEndianImplicit ||
            dataset.getOriginalXfer() == EXS_BigEndianImplicit ||
            dataset.getOriginalXfer() == EXS_LittleEndianExplicit ||
            dataset.getOriginalXfer() == EXS_BigEndianExplicit);
  }


  template <typename PixelType>
  static void CopyPixels(ImageAccessor& target,
                         const DicomIntegerPixelAccessor& source)
  {
    const PixelType minValue = std::numeric_limits<PixelType>::min();
    const PixelType maxValue = std::numeric_limits<PixelType>::max();

    for (unsigned int y = 0; y < source.GetHeight(); y++)
    {
      PixelType* pixel = reinterpret_cast<PixelType*>(target.GetRow(y));
      for (unsigned int x = 0; x < source.GetWidth(); x++)
      {
        for (unsigned int c = 0; c < source.GetChannelCount(); c++, pixel++)
        {
          int32_t v = source.GetValue(x, y, c);
          if (v < static_cast<int32_t>(minValue))
          {
            *pixel = minValue;
          }
          else if (v > static_cast<int32_t>(maxValue))
          {
            *pixel = maxValue;
          }
          else
          {
            *pixel = static_cast<PixelType>(v);
          }
        }
      }
    }
  }


  void DicomImageDecoder::DecodeUncompressedImage(ImageBuffer& target,
                                                  DcmDataset& dataset,
                                                  unsigned int frame)
  {
    if (!IsUncompressedImage(dataset))
    {
      throw OrthancException(ErrorCode_BadParameterType);
    }

    DecodeUncompressedImageInternal(target, dataset, frame);
  }


  void DicomImageDecoder::DecodeUncompressedImageInternal(ImageBuffer& target,
                                                          DcmDataset& dataset,
                                                          unsigned int frame)
  {
    // See also: http://support.dcmtk.org/wiki/dcmtk/howto/accessing-compressed-data

    std::auto_ptr<DicomIntegerPixelAccessor> source;

    DicomMap m;
    FromDcmtkBridge::Convert(m, dataset);


    /**
     * Create an accessor to the raw values of the DICOM image.
     **/

    std::string privateContent;

    DcmElement* e;
    if (dataset.findAndGetElement(ToDcmtkBridge::Convert(DICOM_TAG_PIXEL_DATA), e).good() &&
        e != NULL)
    {
      Uint8* pixData = NULL;
      if (e->getUint8Array(pixData) == EC_Normal)
      {    
        source.reset(new DicomIntegerPixelAccessor(m, pixData, e->getLength()));
      }
    }
    else if (DicomImageDecoder::DecodePsmctRle1(privateContent, dataset))
    {
      LOG(INFO) << "The PMSCT_RLE1 decoding has succeeded";
      Uint8* pixData = NULL;
      if (privateContent.size() > 0)
      {
        pixData = reinterpret_cast<Uint8*>(&privateContent[0]);
      }

      source.reset(new DicomIntegerPixelAccessor(m, pixData, privateContent.size()));
    }
    
    if (source.get() == NULL)
    {
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    source->SetCurrentFrame(frame);


    /**
     * Resize the target image, with some sanity checks.
     **/

    SetupImageBuffer(target, dataset);

    if (target.GetWidth() != target.GetWidth() ||
        target.GetHeight() != target.GetHeight())
    {
      throw OrthancException(ErrorCode_InternalError);
    }

    bool ok;
    switch (target.GetFormat())
    {
      case PixelFormat_RGB24:
        ok = source->GetChannelCount() == 3;
        break;

      case PixelFormat_RGBA32:
        ok = source->GetChannelCount() == 4;
        break;

      case PixelFormat_Grayscale8:
      case PixelFormat_Grayscale16:
      case PixelFormat_SignedGrayscale16:
        ok = source->GetChannelCount() == 1;
        break;

      default:
        ok = false;   // (*)
        break;
    }

    if (!ok)
    {
      throw OrthancException(ErrorCode_InternalError);
    }


    /**
     * Loop over the DICOM buffer, storing its value into the target
     * image.
     **/

    ImageAccessor accessor(target.GetAccessor());

    switch (target.GetFormat())
    {
      case PixelFormat_RGB24:
      case PixelFormat_RGBA32:
      case PixelFormat_Grayscale8:
        CopyPixels<uint8_t>(accessor, *source);
        break;

      case PixelFormat_Grayscale16:
        CopyPixels<uint16_t>(accessor, *source);
        break;

      case PixelFormat_SignedGrayscale16:
        CopyPixels<int16_t>(accessor, *source);
        break;

      default:
        throw OrthancException(ErrorCode_InternalError);
    }
  }


#if ORTHANC_JPEG_LOSSLESS_ENABLED == 1
  void DicomImageDecoder::DecodeJpegLossless(ImageBuffer& target,
                                             DcmDataset& dataset,
                                             unsigned int frame)
  {
    if (!IsJpegLossless(dataset))
    {
      throw OrthancException(ErrorCode_BadParameterType);
    }

    DcmElement *element = NULL;
    if (!dataset.findAndGetElement(ToDcmtkBridge::Convert(DICOM_TAG_PIXEL_DATA), element).good())
    {
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    DcmPixelData& pixelData = dynamic_cast<DcmPixelData&>(*element);
    DcmPixelSequence* pixelSequence = NULL;
    if (!pixelData.getEncapsulatedRepresentation
        (dataset.getOriginalXfer(), NULL, pixelSequence).good())
    {
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    SetupImageBuffer(target, dataset);

    ImageAccessor accessor(target.GetAccessor());

    /**
     * The "DJLSLosslessDecoder" and "DJLSNearLosslessDecoder" in DCMTK
     * are exactly the same, except for the "supportedTransferSyntax()"
     * virtual function.
     * http://support.dcmtk.org/docs/classDJLSDecoderBase.html
     **/

    DJLSLosslessDecoder decoder; DJLSCodecParameter parameters;
    //DJLSNearLosslessDecoder decoder; DJLSCodecParameter parameters;

    Uint32 startFragment = 0;  // Default 
    OFString decompressedColorModel;  // Out
    DJ_RPLossless representationParameter;
    OFCondition c = decoder.decodeFrame(&representationParameter, pixelSequence, &parameters, 
                                        &dataset, frame, startFragment, accessor.GetBuffer(), 
                                        accessor.GetSize(), decompressedColorModel);

    if (!c.good())
    {
      throw OrthancException(ErrorCode_InternalError);
    }
  }
#endif



  bool DicomImageDecoder::Decode(ImageBuffer& target,
                                 DcmDataset& dataset,
                                 unsigned int frame)
  {
    if (IsUncompressedImage(dataset))
    {
      DecodeUncompressedImage(target, dataset, frame);
      return true;
    }

#if ORTHANC_JPEG_LOSSLESS_ENABLED == 1
    if (IsJpegLossless(dataset))
    {
      LOG(INFO) << "Decoding a JPEG-LS image";
      DecodeJpegLossless(target, dataset, frame);
      return true;
    }
#endif

    /**
     * This DICOM image format is not natively supported by
     * Orthanc. As a last resort, try and decode it through
     * DCMTK. This will result in higher memory consumption. This is
     * actually the second example of the following page:
     * http://support.dcmtk.org/docs/mod_dcmjpeg.html#Examples
     **/
    
    {
      LOG(INFO) << "Using DCMTK to decode a compressed image";

      std::auto_ptr<DcmDataset> converted(dynamic_cast<DcmDataset*>(dataset.clone()));
      converted->chooseRepresentation(EXS_LittleEndianExplicit, NULL);

      if (converted->canWriteXfer(EXS_LittleEndianExplicit))
      {
        DecodeUncompressedImageInternal(target, *converted, frame);
        return true;
      }
    }

    return false;
  }
}