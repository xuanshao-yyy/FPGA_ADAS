#pragma once
#include "signal_control.hpp"
#include "input_source.hpp"
#include <filesystem>
#include <set>
namespace scene_player {
struct Scene {std::string id,file,config;double fps=0;};
inline std::vector<Scene> load(const std::string& path,unsigned max_mib){
    cv::FileStorage fs(path,cv::FileStorage::READ);if(!fs.isOpened())throw std::runtime_error("Cannot open playlist");
    auto nodes=fs["scenes"];if(nodes.type()!=cv::FileNode::SEQ || nodes.empty())throw std::runtime_error("Empty playlist");
    std::vector<Scene> scenes;std::set<std::string> ids;
    for(auto node:nodes){Scene s;node["id"]>>s.id;node["file"]>>s.file;node["config"]>>s.config;node["fps"]>>s.fps;
        if(s.id.empty() || s.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos || !ids.insert(s.id).second || !std::isfinite(s.fps)||s.fps<1||s.fps>120)
            throw std::runtime_error("Invalid playlist scene/fps");
        const auto bytes=qv::MemoryVideo::validated_size(s.file);
        if(bytes>uint64_t(max_mib)*1024*1024)throw std::runtime_error("Playlist clip exceeds local file size guard");
        auto cfg=signal_demo::load(s.config);
        if(cfg.loop_frames && cfg.loop_frames!=bytes/qv::FrameBytes)throw std::runtime_error("Playlist loop length mismatch");
        scenes.push_back(s);
    }
    return scenes;
}
}
