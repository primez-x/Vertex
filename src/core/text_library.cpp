#include "sketch/text_library.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {
using json=nlohmann::json;

void check(bool condition,const char* message) {
    if(!condition) throw std::invalid_argument(message);
}
void fields(const json& value,std::initializer_list<std::string_view> allowed) {
    check(value.is_object(),"Text library record must be an object.");
    check(value.size()==allowed.size(),"Text library has missing or unknown fields.");
    for(const auto key:allowed) check(value.contains(std::string(key)),"Text library has missing or unknown fields.");
}
void text(const std::string& value,std::size_t limit,bool multiline,const char* message) {
    check(!value.empty() && value.size()<=limit,message);
    bool meaningful=false;
    for(const unsigned char character:value) {
        check(character>=32 || (multiline && (character=='\n' || character=='\r' || character=='\t')),message);
        check(character!=127,message);
        if(!std::isspace(character)) meaningful=true;
    }
    check(meaningful,message);
    // JSON's strict UTF-8 encoder rejects invalid byte sequences before any save.
    (void)json(value).dump();
}
AnnotationState carrier(const TextLibraryDocument& document) {
    check(document.version==1 || document.version==2,"Unsupported text library version. Use a compatible application.");
    check(document.entries.size()<=kTextLibraryEntryLimit,"Text library exceeds 1000 entries.");
    AnnotationState state;
    std::set<std::string> ids;
    const auto builtins=default_label_templates();
    for(const auto& entry:document.entries) {
        check(!entry.id.empty() && entry.id.size()<=256 &&
            entry.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:")==std::string::npos,
            "Text entry ID must be a bounded stable token.");
        check(ids.insert(entry.id).second,"Text library contains duplicate entry IDs.");
        check(std::none_of(builtins.begin(),builtins.end(),[&](const auto& value){return value.id==entry.id;}),
            "Text entry ID is reserved for a built-in template. Use a unique user-text ID.");
        text(entry.name,256,false,"Text entry name must contain readable text (maximum 256 bytes).");
        text(entry.category,256,false,"Text entry category must contain readable text (maximum 256 bytes).");
        text(entry.content,65536,true,"Text entry content must contain readable text (maximum 65536 bytes).");
        text(entry.style.font_family,256,false,"Text entry font family must contain readable text.");
        check(entry.style.text_height_metres<=100 && entry.style.stroke_width_metres<=1,
            "Text style dimensions exceed the reusable text limits (100 m height, 1 m stroke).");
        LabelInstance label;
        label.id=entry.id;
        label.template_id="free-text";
        label.content=entry.content;
        label.style=entry.style;
        state.labels.push_back(std::move(label));
    }
    validate_annotation_state(state,{});
    return state;
}
}

json encode_text_library(const TextLibraryDocument& document) {
    try {
        const auto annotations=encode_annotation_state(carrier(document),{});
        const bool aligned=document.version==2 || std::any_of(document.entries.begin(),document.entries.end(),
            [](const auto& e){return e.style.text_alignment!="center";});
        json encoded{{"version",aligned?2:1},{"entries",json::array()}};
        for(std::size_t i=0;i<document.entries.size();++i) {
            const auto& entry=document.entries[i];
            encoded["entries"].push_back({{"id",entry.id},{"name",entry.name},
                {"category",entry.category},{"content",entry.content},
                {"style",annotations.at("labels").at(i).at("style")}});
            if(aligned)encoded["entries"].back()["style"]["text_alignment"]=entry.style.text_alignment;
        }
        check(encoded.dump().size()<=kTextLibraryByteLimit,"Text library exceeds its 4 MiB storage limit.");
        return encoded;
    } catch(const json::exception& error) {
        throw std::invalid_argument(std::string("Invalid text library: ")+error.what());
    }
}

void validate_text_library(const TextLibraryDocument& document) {
    (void)encode_text_library(document);
}

TextLibraryDocument decode_text_library(const json& encoded) {
    try {
        fields(encoded,{"version","entries"});
        check(encoded.at("version").is_number_integer() && (encoded.at("version")==1 || encoded.at("version")==2),
            "Unsupported text library version. Use a compatible application.");
        check(encoded.at("entries").is_array() && encoded.at("entries").size()<=kTextLibraryEntryLimit,
            "Text library entries must be an array with at most 1000 records.");
        check(encoded.dump().size()<=kTextLibraryByteLimit,"Text library exceeds its 4 MiB storage limit.");
        auto annotations=encode_annotation_state(AnnotationState{},{});
        const bool aligned=encoded.at("version")==2;
        // A v2 library uses a v8 style carrier even when all text is centered;
        // encoding an empty/default annotation state would select legacy v3.
        if(aligned)annotations["version"]=8;
        LabelInstance prototype;
        prototype.id="text-style-carrier";
        AnnotationState prototype_state;
        prototype_state.labels.push_back(prototype);
        const auto prototype_wire=encode_annotation_state(prototype_state,{}).at("labels").at(0);
        TextLibraryDocument document;
        document.version=encoded.at("version").get<int>();
        for(const auto& record:encoded.at("entries")) {
            fields(record,{"id","name","category","content","style"});
            if(aligned)fields(record.at("style"),{"font_family","text_height_metres","stroke_width_metres",
                "stroke_color","fill_color","fill_pattern","bold","italic","text_alignment"});
            else fields(record.at("style"),{"font_family","text_height_metres","stroke_width_metres",
                "stroke_color","fill_color","fill_pattern","bold","italic"});
            TextLibraryEntry entry;
            entry.id=record.at("id").get<std::string>();
            entry.name=record.at("name").get<std::string>();
            entry.category=record.at("category").get<std::string>();
            entry.content=record.at("content").get<std::string>();
            auto label=prototype_wire;
            label["style"]=record.at("style");
            annotations["labels"]=json::array({label});
            entry.style=decode_annotation_state(annotations,{}).labels.at(0).style;
            document.entries.push_back(std::move(entry));
        }
        validate_text_library(document);
        return document;
    } catch(const json::exception& error) {
        throw std::invalid_argument(std::string("Invalid text library: ")+error.what());
    }
}
} // namespace sketch
