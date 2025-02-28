#include "opendrive-engine/convertor.h"

#include <algorithm>
#include <memory>
#include <string>

#include "cactus/factory.h"
#include "opendrive-cpp/common/common.hpp"
#include "opendrive-engine/common/log.h"
#include "opendrive-engine/core/lane.h"

namespace opendrive {
namespace engine {

inline void Convertor::SetStatus(ErrorCode code, const std::string& msg) {
  status_.error_code = code;
  status_.msg = msg;
}

inline bool Convertor::Continue() const {
  return ErrorCode::OK == status_.error_code;
}

Status Convertor::Start() {
  auto factory = cactus::Factory::Instance();
  param_ = factory->GetObject<common::Param>("engine_param");
  data_ = factory->GetObject<core::Data>("core_data");
  center_line_pts_.clear();
  if (!param_ || !data_) {
    SetStatus(ErrorCode::INIT_FACTORY_ERROR, "factory error.");
    return status_;
  }
  step_ = std::max<float>(0.1, param_->step);
  status_.error_code = ErrorCode::OK;
  status_.msg = "ok";
  std::string map_file = param_->map_file;
  if (map_file.empty() || !cactus::FileExists(map_file) || !data_) {
    SetStatus(ErrorCode::INIT_MAPFILE_ERROR, "input file error: " + map_file);
    return status_;
  }
  std::unique_ptr<opendrive::Parser> perser =
      std::make_unique<opendrive::Parser>();
  opendrive::element::Map::Ptr ele_map =
      std::make_shared<opendrive::element::Map>();
  auto parse_ret = perser->ParseMap(map_file, ele_map);
  if (opendrive::ErrorCode::OK != parse_ret.error_code) {
    SetStatus(ErrorCode::INIT_MAPFILE_ERROR, "input file error: " + map_file);
    return status_;
  }

  ConvertHeader(ele_map)
      .ConvertRoad(ele_map)
      .ConvertJunction(ele_map)
      .BuildKDTree()
      .End();

  return status_;
}

void Convertor::End() {
  if (!Continue()) return;
  core::Curve::Points().swap(center_line_pts_);
}

Convertor& Convertor::ConvertHeader(opendrive::element::Map::Ptr ele_map) {
  if (!Continue()) return *this;
  ENGINE_INFO("Convert Header Start")
  auto header = std::make_shared<core::Header>();
  header->set_rev_major(ele_map->header().rev_major());
  header->set_rev_minor(ele_map->header().rev_minor());
  header->set_name(ele_map->header().name());
  header->set_version(ele_map->header().version());
  header->set_date(ele_map->header().date());
  header->set_north(ele_map->header().north());
  header->set_south(ele_map->header().south());
  header->set_west(ele_map->header().west());
  header->set_east(ele_map->header().east());
  header->set_vendor(ele_map->header().vendor());
  data_->set_header(header);
  ENGINE_INFO("Convert Header End")
  return *this;
}

Convertor& Convertor::ConvertJunction(opendrive::element::Map::Ptr ele_map) {
  if (!Continue()) return *this;
  ENGINE_INFO("Convert Junction Start")
  for (const auto& ele_junction : ele_map->junctions()) {
    if (ele_junction.attribute().id() < 0) continue;
    auto junction = std::make_shared<core::Junction>();
    ConvertJunctionAttr(ele_junction, junction);
    data_->mutable_junction()[junction->id()] = junction;
  }
  ENGINE_INFO("Convert Junction End")
  return *this;
}

Convertor& Convertor::ConvertJunctionAttr(const element::Junction& ele_junction,
                                          core::Junction::Ptr junction) {
  if (!Continue()) return *this;
  junction->set_id(std::to_string(ele_junction.attribute().id()));
  junction->set_name(ele_junction.attribute().name());
  junction->set_type(ele_junction.attribute().type());
  return *this;
}

Convertor& Convertor::ConvertRoad(opendrive::element::Map::Ptr ele_map) {
  if (!Continue()) return *this;
  ENGINE_INFO("Convert Road Start")
  for (const auto& ele_road : ele_map->roads()) {
    if (ele_road.attribute().id() < 0) continue;
    auto road = std::make_shared<core::Road>();
    ConvertRoadAttr(ele_road, road).ConvertSection(ele_road, road);
    data_->mutable_roads()[road->id()] = road;
  }
  ENGINE_INFO("Convert Road End")
  return *this;
}

Convertor& Convertor::ConvertRoadAttr(const element::Road& ele_road,
                                      core::Road::Ptr road) {
  if (!Continue()) return *this;
  road->set_id(std::to_string(ele_road.attribute().id()));
  road->set_name(ele_road.attribute().name());
  road->set_junction_id(std::to_string(ele_road.attribute().junction_id()));
  road->set_length(ele_road.attribute().length());
  road->set_rule(ele_road.attribute().rule());
  if (-1 != ele_road.link().predecessor().id()) {
    // road predecessor/successor range 0~1 in opendrive.
    road->mutable_predecessor_ids().emplace(
        std::to_string(ele_road.link().predecessor().id()));
  }
  if (-1 != ele_road.link().successor().id()) {
    road->mutable_successor_ids().emplace(
        std::to_string(ele_road.link().successor().id()));
  }
  for (const auto& ele_info : ele_road.type_info()) {
    core::RoadInfo road_info;
    road_info.set_s(ele_info.start_position());
    road_info.set_type(ele_info.type());
    road->mutable_info().emplace_back(road_info);
  }
  return *this;
}

Convertor& Convertor::ConvertSection(const element::Road& ele_road,
                                     core::Road::Ptr road) {
  if (!Continue()) return *this;

  std::string road_id = std::to_string(ele_road.attribute().id());
  double road_ds = 0;
  int section_idx = 0;
  for (const auto& ele_section : ele_road.lanes().lane_sections()) {
    auto section = std::make_shared<core::Section>();
    road->mutable_sections().emplace_back(section);
    section->set_id(road_id + "_" + std::to_string(section_idx++));
    section->set_parent_id(road_id);
    section->set_start_position(ele_section.start_position());
    section->set_end_position(ele_section.end_position());
    section->set_length(ele_section.end_position() -
                        ele_section.start_position());
    data_->mutable_sections()[section->id()] = section;

    /// center lane
    if (1 != ele_section.center().lanes().size()) {
      SetStatus(ErrorCode::CONVERTOR_CENTERLANE_ERROR,
                section->id() + " center lane size not equal 1.");
      return *this;
    } else {
      auto lane = std::make_shared<core::Lane>();
      section->mutable_center_lane() = lane;
      // lane attr
      lane->set_id(section->id() + "_0");
      lane->set_parent_id(section->id());
      CenterLaneSampling(ele_road.plan_view().geometrys(),
                         ele_road.lanes().lane_offsets(), section, road_ds);
      data_->mutable_lanes()[lane->id()] = lane;
    }
    // 参考线: 中心车道的左边界
    core::Curve::Line& refe_line = section->mutable_center_lane()
                                       ->mutable_left_boundary()
                                       .mutable_curve()
                                       .mutable_pts();

    /// left lanes
    for (const auto& ele_lane : ele_section.left().lanes()) {
      auto lane = std::make_shared<core::Lane>();
      section->mutable_left_lanes().emplace_back(lane);
      lane->set_id(section->id() + "_" +
                   std::to_string(ele_lane.attribute().id()));
      lane->set_parent_id(section->id());
      LaneSampling(ele_lane, lane, refe_line);
      data_->mutable_lanes()[lane->id()] = lane;
      refe_line = lane->mutable_right_boundary().mutable_curve().mutable_pts();
    }
    // 参考线: 中心车道的右边界
    refe_line = section->mutable_center_lane()
                    ->mutable_right_boundary()
                    .mutable_curve()
                    .mutable_pts();

    /// right lanes
    for (const auto& ele_lane : ele_section.right().lanes()) {
      auto lane = std::make_shared<core::Lane>();
      section->mutable_right_lanes().emplace_back(lane);
      lane->set_id(section->id() + "_" +
                   std::to_string(ele_lane.attribute().id()));
      lane->set_parent_id(section->id());
      LaneSampling(ele_lane, lane, refe_line);
      data_->mutable_lanes()[lane->id()] = lane;
      refe_line = lane->mutable_right_boundary().mutable_curve().mutable_pts();
    }
  }

  return *this;
}

Convertor& Convertor::BuildKDTree() {
  if (!Continue()) return *this;
  auto factory = cactus::Factory::Instance();
  auto kdtree = factory->GetObject<kdtree::KDTree>("kdtree");
  kdtree->Init(center_line_pts_);
  return *this;
}

void Convertor::AppendKDTreeSample(const core::Curve::Point& point) {
  center_line_pts_.emplace_back(point);
}

void Convertor::CenterLaneSampling(
    const element::Geometry::ConstPtrs& geometrys,
    const element::LaneOffsets& lane_offsets, core::Section::Ptr section,
    double& road_ds) {
  double section_ds = 0;
  core::Curve::Point point;
  element::Point refe_point;
  element::Point offset_point;
  element::Geometry::ConstPtr geometry = nullptr;
  int geometry_type = -1;
  size_t point_idx = 0;
  section->mutable_center_lane()->mutable_central_curve().mutable_pts().clear();

  while (true) {
    if (section_ds > section->length()) {
      // TODO: section_ds - section->length() 不等于 step_
      if (section_ds - section->length() >= step_ - 1e-10) {
        break;
      } else {
        section_ds = section->length();
        road_ds = road_ds - (section_ds - section->length());
      }
    }
    geometry = GetGeometry(geometrys, road_ds);
    if (!geometry) {
      break;
    }
    refe_point = geometry->GetPoint(road_ds);
    double offset = GetLaneOffsetValue(lane_offsets, road_ds);
    if (0 != offset) {
      offset_point =
          opendrive::common::GetOffsetPoint<element::Point>(refe_point, offset);
      point.mutable_x() = offset_point.x();
      point.mutable_y() = offset_point.y();
      point.mutable_heading() = offset_point.heading();
      point.mutable_start_position() = section_ds;
      point.mutable_id() =
          section->center_lane()->id() + "_" + std::to_string(point_idx++);
    } else {
      point.mutable_x() = refe_point.x();
      point.mutable_y() = refe_point.y();
      point.mutable_heading() = refe_point.heading();
      point.mutable_start_position() = section_ds;
      point.mutable_id() =
          section->center_lane()->id() + "_" + std::to_string(point_idx++);
    }
    if (geometry_type != static_cast<int>(geometry->type())) {
      // new geometry
      core::Geomotry core_geo;
      core_geo.set_type(geometry->type());
      core_geo.set_point(point);
      section->mutable_center_lane()->mutable_geometrys().emplace_back(
          core_geo);
      geometry_type = static_cast<int>(geometry->type());
    }
    section->mutable_center_lane()
        ->mutable_central_curve()
        .mutable_pts()
        .emplace_back(point);
    section->mutable_center_lane()
        ->mutable_left_boundary()
        .mutable_curve()
        .mutable_pts()
        .emplace_back(point);
    section->mutable_center_lane()
        ->mutable_right_boundary()
        .mutable_curve()
        .mutable_pts()
        .emplace_back(point);

    section_ds += step_;
    road_ds += step_;
  }
}

void Convertor::LaneSampling(const element::Lane& ele_lane,
                             core::Lane::Ptr lane,
                             const core::Curve::Line& refe_line) {
  core::Curve::Point point;
  int point_idx = 0;
  auto lane_idx = opendrive::common::Split(lane->id(), "_");
  const int lane_dir = lane_idx.at(2) > "0" ? 1 : -1;
  double lane_width = 0;
  core::Id point_id = "";
  for (const auto& refe_point : refe_line) {
    lane_width = ele_lane.GetLaneWidth(refe_point.start_position()) * lane_dir;
    point_id = lane->id() + "_" + std::to_string(point_idx++);

    // left boundary point
    point = refe_point;
    point.mutable_id() = point_id + "_1";
    lane->mutable_left_boundary().mutable_curve().mutable_pts().emplace_back(
        point);

    // center line point
    point = opendrive::common::GetOffsetPoint<core::Curve::Point>(
        refe_point, lane_width / 2.0);
    point.mutable_id() = point_id + "_2";
    lane->mutable_central_curve().mutable_pts().emplace_back(point);
    AppendKDTreeSample(point);

    // right boundary point
    point = opendrive::common::GetOffsetPoint<core::Curve::Point>(refe_point,
                                                                  lane_width);
    point.mutable_id() = point_id + "_3";
    lane->mutable_right_boundary().mutable_curve().mutable_pts().emplace_back(
        point);
  }
}

element::Geometry::ConstPtr Convertor::GetGeometry(
    const element::Geometry::ConstPtrs& geometrys, double road_ds) {
  auto geometry_idx = opendrive::common::GetGtPtrPoloy3(geometrys, road_ds);
  if (geometry_idx < 0) {
    SetStatus(ErrorCode::CONVERTOR_CENTERLANE_ERROR,
              "get geometry index execption.");
    return nullptr;
  }
  if (geometry_idx < geometrys.size()) {
    return geometrys.at(geometry_idx);
  }
  return nullptr;
}

double Convertor::GetLaneOffsetValue(const element::LaneOffsets& offsets,
                                     double road_ds) {
  int offset_idx = opendrive::common::GetGeValuePoloy3(offsets, road_ds);
  if (offset_idx >= 0 && offset_idx < offsets.size()) {
    return offsets.at(offset_idx).GetOffsetValue(road_ds);
  }
  return 0;
}

}  // namespace engine
}  // namespace opendrive
