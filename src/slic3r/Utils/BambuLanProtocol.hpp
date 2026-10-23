#pragma once

#include "bambu_networking.hpp"
#include <nlohmann/json.hpp>
#include <istream>
#include <cstddef>
#include <ios>
#include <streambuf>
#include <string>

namespace Slic3r::BambuLanProtocol {
constexpr size_t max_frame_bytes = 4 * 1024 * 1024;
// JSON strings escape embedded newlines. Bound the frame before allocating an
// arbitrarily large message, and never log a frame: credentials travel here.
// std::getline would buffer an unbounded line before the check, so read the
// stream buffer directly instead of one formatted extraction per byte.
inline bool read_frame(std::istream& stream, nlohmann::json& frame)
{
    const std::istream::sentry sentry(stream, true);
    std::streambuf* buffer = sentry ? stream.rdbuf() : nullptr;
    if (!buffer) return false;
    std::string line;
    try {
        for (auto ch = buffer->sbumpc(); ch != std::char_traits<char>::eof(); ch = buffer->sbumpc()) {
            if (ch == '\n') {
                frame = nlohmann::json::parse(line, nullptr, false);
                return frame.is_object();
            }
            if (line.size() == max_frame_bytes) return false;
            line.push_back(std::char_traits<char>::to_char_type(ch));
        }
        stream.setstate(std::ios::eofbit);
    } catch (...) {
        // A pipe read error ends the stream like EOF; it never escapes as data.
        stream.setstate(std::ios::badbit);
    }
    return false;
}

inline nlohmann::json encode_print(const PrintParams& p)
{
    nlohmann::json j;
    j["dev_id"] = p.dev_id;
    j["task_name"] = p.task_name;
    j["project_name"] = p.project_name;
    j["preset_name"] = p.preset_name;
    j["filename"] = p.filename;
    j["config_filename"] = p.config_filename;
    j["plate_index"] = p.plate_index;
    j["ftp_folder"] = p.ftp_folder;
    j["ftp_file"] = p.ftp_file;
    j["ftp_file_md5"] = p.ftp_file_md5;
    j["nozzle_mapping"] = p.nozzle_mapping;
    j["ams_mapping"] = p.ams_mapping;
    j["ams_mapping2"] = p.ams_mapping2;
    j["ams_mapping_info"] = p.ams_mapping_info;
    j["nozzles_info"] = p.nozzles_info;
    j["connection_type"] = p.connection_type;
    j["comments"] = p.comments;
    j["origin_profile_id"] = p.origin_profile_id;
    j["stl_design_id"] = p.stl_design_id;
    j["origin_model_id"] = p.origin_model_id;
    j["print_type"] = p.print_type;
    j["dst_file"] = p.dst_file;
    j["dev_name"] = p.dev_name;
    j["dev_ip"] = p.dev_ip;
    j["use_ssl_for_ftp"] = p.use_ssl_for_ftp;
    j["use_ssl_for_mqtt"] = p.use_ssl_for_mqtt;
    j["username"] = p.username;
    j["password"] = p.password;
    j["task_bed_leveling"] = p.task_bed_leveling;
    j["task_flow_cali"] = p.task_flow_cali;
    j["task_vibration_cali"] = p.task_vibration_cali;
    j["task_layer_inspect"] = p.task_layer_inspect;
    j["task_record_timelapse"] = p.task_record_timelapse;
    j["task_timelapse_use_internal"] = p.task_timelapse_use_internal;
    j["task_use_ams"] = p.task_use_ams;
    j["task_bed_type"] = p.task_bed_type;
    j["extra_options"] = p.extra_options;
    j["auto_bed_leveling"] = p.auto_bed_leveling;
    j["auto_flow_cali"] = p.auto_flow_cali;
    j["auto_offset_cali"] = p.auto_offset_cali;
    j["extruder_cali_manual_mode"] = p.extruder_cali_manual_mode;
    j["task_ext_change_assist"] = p.task_ext_change_assist;
    j["try_emmc_print"] = p.try_emmc_print;
    j["svc_context"] = p.svc_context;
    j["slicer_uid"] = p.slicer_uid;
    j["queue_plate_id"] = p.queue_plate_id;
    return j;
}
inline PrintParams decode_print(const nlohmann::json& j)
{
    PrintParams p{};
    j.at("dev_id").get_to(p.dev_id);
    j.at("task_name").get_to(p.task_name);
    j.at("project_name").get_to(p.project_name);
    j.at("preset_name").get_to(p.preset_name);
    j.at("filename").get_to(p.filename);
    j.at("config_filename").get_to(p.config_filename);
    j.at("plate_index").get_to(p.plate_index);
    j.at("ftp_folder").get_to(p.ftp_folder);
    j.at("ftp_file").get_to(p.ftp_file);
    j.at("ftp_file_md5").get_to(p.ftp_file_md5);
    j.at("nozzle_mapping").get_to(p.nozzle_mapping);
    j.at("ams_mapping").get_to(p.ams_mapping);
    j.at("ams_mapping2").get_to(p.ams_mapping2);
    j.at("ams_mapping_info").get_to(p.ams_mapping_info);
    j.at("nozzles_info").get_to(p.nozzles_info);
    j.at("connection_type").get_to(p.connection_type);
    j.at("comments").get_to(p.comments);
    j.at("origin_profile_id").get_to(p.origin_profile_id);
    j.at("stl_design_id").get_to(p.stl_design_id);
    j.at("origin_model_id").get_to(p.origin_model_id);
    j.at("print_type").get_to(p.print_type);
    j.at("dst_file").get_to(p.dst_file);
    j.at("dev_name").get_to(p.dev_name);
    j.at("dev_ip").get_to(p.dev_ip);
    j.at("use_ssl_for_ftp").get_to(p.use_ssl_for_ftp);
    j.at("use_ssl_for_mqtt").get_to(p.use_ssl_for_mqtt);
    j.at("username").get_to(p.username);
    j.at("password").get_to(p.password);
    j.at("task_bed_leveling").get_to(p.task_bed_leveling);
    j.at("task_flow_cali").get_to(p.task_flow_cali);
    j.at("task_vibration_cali").get_to(p.task_vibration_cali);
    j.at("task_layer_inspect").get_to(p.task_layer_inspect);
    j.at("task_record_timelapse").get_to(p.task_record_timelapse);
    j.at("task_timelapse_use_internal").get_to(p.task_timelapse_use_internal);
    j.at("task_use_ams").get_to(p.task_use_ams);
    j.at("task_bed_type").get_to(p.task_bed_type);
    j.at("extra_options").get_to(p.extra_options);
    j.at("auto_bed_leveling").get_to(p.auto_bed_leveling);
    j.at("auto_flow_cali").get_to(p.auto_flow_cali);
    j.at("auto_offset_cali").get_to(p.auto_offset_cali);
    j.at("extruder_cali_manual_mode").get_to(p.extruder_cali_manual_mode);
    j.at("task_ext_change_assist").get_to(p.task_ext_change_assist);
    j.at("try_emmc_print").get_to(p.try_emmc_print);
    j.at("svc_context").get_to(p.svc_context);
    j.at("slicer_uid").get_to(p.slicer_uid);
    j.at("queue_plate_id").get_to(p.queue_plate_id);
    return p;
}
} // namespace Slic3r::BambuLanProtocol
