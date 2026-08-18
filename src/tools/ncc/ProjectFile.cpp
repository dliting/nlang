/*--- ProjectFile.cpp - .nproj project file loader for ncc (-p mode) ---*/
#include "ProjectFile.h"

#include "tinyxml2.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;

namespace nlang {

//Dedup key for <File> entries: lexically normalized (folding "sub/../"),
//and on Windows case-folded to match NTFS semantics — entries naming the
//same physical file must collide here, not just identical raw strings.
static std::string SourceKey(const fs::path& abs) {
    std::string key = abs.lexically_normal().string();
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
#endif
    return key;
}

bool ProjectFile::Load(const std::string& projectPath, ProjectFile& out,
                       std::string& errorMessage) {
    fs::path proj(projectPath);
    if (!fs::exists(proj)) {
        errorMessage = "project file not found: " + projectPath;
        return false;
    }

    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(projectPath.c_str()) != tinyxml2::XML_SUCCESS) {
        errorMessage = "XML parse error in " + projectPath + ": "
            + doc.ErrorStr();
        return false;
    }

    const tinyxml2::XMLElement* root = doc.RootElement();
    if (!root || std::string(root->Name()) != "Project") {
        errorMessage = "not a project file (root element must be <Project>): "
            + projectPath;
        return false;
    }

    out = ProjectFile();
    out.projectDir = fs::absolute(proj).parent_path().string();
    out.name = root->Attribute("name") ? root->Attribute("name") : "";
    if (out.name.empty()) {
        //Default to the file stem ("hello.nproj" -> "hello").
        out.name = proj.stem().string();
    }
    out.outputDir = root->Attribute("outputDir")
        ? root->Attribute("outputDir") : "";

    const tinyxml2::XMLElement* sources = root->FirstChildElement("Sources");
    if (!sources) {
        errorMessage = "project file has no <Sources> element: " + projectPath;
        return false;
    }
    //A second <Sources> block is a schema violation: its files would be
    //silently dropped, so reject rather than ignore.
    if (sources->NextSiblingElement("Sources")) {
        errorMessage = "project file has more than one <Sources> element: "
            + projectPath;
        return false;
    }

    std::set<std::string> seen;  //duplicate entries are a user mistake
    for (const tinyxml2::XMLElement* file = sources->FirstChildElement("File");
         file; file = file->NextSiblingElement("File")) {
        const char* path = file->Attribute("path");
        if (!path || !*path) {
            errorMessage = "<File> entry without a path attribute in "
                + projectPath;
            return false;
        }
        fs::path abs = fs::absolute(fs::path(out.projectDir) / path)
            .lexically_normal();
        if (!seen.insert(SourceKey(abs)).second) {
            errorMessage = "duplicate source entry: " + abs.string();
            return false;
        }
        out.sources.push_back(abs.string());
    }
    if (out.sources.empty()) {
        errorMessage = "project has no source files: " + projectPath;
        return false;
    }

    return true;
}

} //namespace nlang
