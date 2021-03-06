/*
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2009, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage, Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <ecto/ecto.hpp>

#include <boost/foreach.hpp>

#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/objdetect/objdetect.hpp>

#include <object_recognition_core/db/ModelReader.h>
#include <object_recognition_core/common/pose_result.h>

#include "db_linemod.h"

using ecto::tendrils;
using ecto::spore;
using object_recognition_core::db::ObjectId;
using object_recognition_core::common::PoseResult;
using object_recognition_core::db::ObjectDbPtr;

void
drawResponse(const std::vector<cv::linemod::Template>& templates, int num_modalities, cv::Mat& dst, cv::Point offset,
             int T)
{
  static const cv::Scalar COLORS[5] =
  { CV_RGB(0, 0, 255), CV_RGB(0, 255, 0), CV_RGB(255, 255, 0), CV_RGB(255, 140, 0), CV_RGB(255, 0, 0) };

  for (int m = 0; m < num_modalities; ++m)
  {
// NOTE: Original demo recalculated max response for each feature in the TxT
// box around it and chose the display color based on that response. Here
// the display color just depends on the modality.
    cv::Scalar color = COLORS[m];

    for (int i = 0; i < (int) templates[m].features.size(); ++i)
    {
      cv::linemod::Feature f = templates[m].features[i];
      cv::Point pt(f.x + offset.x, f.y + offset.y);
      cv::circle(dst, pt, T / 2, color);
    }
  }
}

namespace ecto_linemod
{
struct Detector: public object_recognition_core::db::bases::ModelReaderBase {
  void parameter_callback(
      const object_recognition_core::db::Documents & db_documents) {
    /*if (submethod.get_str() == "DefaultLINEMOD")
     detector_ = cv::linemod::getDefaultLINEMOD();
     else
     throw std::runtime_error("Unsupported method. Supported ones are: DefaultLINEMOD");*/

    detector_ = cv::linemod::getDefaultLINEMOD();
    BOOST_FOREACH(const object_recognition_core::db::Document & document, db_documents) {
      std::string object_id = document.get_field<ObjectId>("object_id");

      // Load the detector for that class
      cv::linemod::Detector detector;
      document.get_attachment<cv::linemod::Detector>("detector", detector);
      std::string object_id_in_db = detector.classIds()[0];
      for (size_t template_id = 0; template_id < detector.numTemplates();
          ++template_id)
        detector_->addSyntheticTemplate(
            detector.getTemplates(object_id_in_db, template_id), object_id);

      // Deal with the poses
      document.get_attachment<std::vector<cv::Mat> >("Rs", Rs_[object_id]);
      document.get_attachment<std::vector<cv::Mat> >("Ts", Ts_[object_id]);

      std::cout << "Loaded " << object_id << std::endl;
    }
  }

    static void
    declare_params(tendrils& params)
    {
      object_recognition_core::db::bases::declare_params_impl(params, "LINEMOD");
      params.declare(&Detector::threshold_, "threshold", "Matching threshold, as a percentage", 93.0f);
      params.declare(&Detector::visualize_, "visualize", "If True, visualize the output.", false);
    }

    static void
    declare_io(const tendrils& params, tendrils& inputs, tendrils& outputs)
    {
      inputs.declare(&Detector::color_, "image", "An rgb full frame image.");
      inputs.declare(&Detector::depth_, "depth", "The 16bit depth image.");

      outputs.declare(&Detector::pose_results_, "pose_results", "The results of object recognition");
    }

    void
    configure(const tendrils& params, const tendrils& inputs, const tendrils& outputs)
    {
      configure_impl();
    }

    int
    process(const tendrils& inputs, const tendrils& outputs)
    {
      // Resize color to 640x480
      /// @todo Move resizing to separate cell, and try LINE-MOD w/ SXGA images
      cv::Mat color;
      if (color_->rows > 960)
        cv::pyrDown(color_->rowRange(0, 960), color);
      else
        color_->copyTo(color);

      pose_results_->clear();

      if (detector_->classIds().empty())
        return ecto::OK;

      std::vector<cv::Mat> sources;
      sources.push_back(color);
      sources.push_back(*depth_);

      std::vector<cv::linemod::Match> matches;
      detector_->match(sources, *threshold_, matches);
      cv::Mat display = color;
      int num_modalities = (int) detector_->getModalities().size();

    BOOST_FOREACH(const cv::linemod::Match & match, matches) {
      const std::vector<cv::linemod::Template>& templates =
          detector_->getTemplates(match.class_id, match.template_id);
      if (*visualize_)
        drawResponse(templates, num_modalities, display,
            cv::Point(match.x, match.y), detector_->getT(0));

      // Fill the Pose object
      PoseResult pose_result;
      cv::Mat R = Rs_.at(match.class_id)[match.template_id].clone();
      cv::Mat T = Ts_.at(match.class_id)[match.template_id].clone();
      T = R*T;
      T.at<double>(1,0) = -T.at<double>(1,0);
      T.at<double>(2,0) = -T.at<double>(2,0);
      pose_result.set_R(R);
      pose_result.set_T(T);
      pose_result.set_object_id(db_, match.class_id);
      pose_results_->push_back(pose_result);
    };
    if (*visualize_) {
      cv::namedWindow("LINEMOD");
      cv::imshow("LINEMOD", display);
      cv::waitKey(1);
    }
    return ecto::OK;
  }

  /** LINE-MOD detector */
  cv::Ptr<cv::linemod::Detector> detector_;
    // Parameters
    spore<float> threshold_;
    // Inputs
    spore<cv::Mat> color_, depth_;

    /** True or False to output debug image */
    ecto::spore<bool> visualize_;
    /** The object recognition results */
    ecto::spore<std::vector<PoseResult> > pose_results_;
    /** The rotations, per object and per template */
    std::map<std::string, std::vector<cv::Mat> > Rs_;
    /** The translations, per object and per template */
    std::map<std::string, std::vector<cv::Mat> > Ts_;
  };

} // namespace ecto_linemod

ECTO_CELL(ecto_linemod, ecto_linemod::Detector, "Detector", "Use LINE-MOD for object detection.")
