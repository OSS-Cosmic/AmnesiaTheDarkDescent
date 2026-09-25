#ifndef HPL_RENDERER_XML_H
#define HPL_RENDERER_XML_H

#include "graphics/RendererMask.h"
#include <tinyxml2.h>
#include <cstring>
#include <string>
#include <vector>

namespace hpl {

inline bool IsRendererXmlVisual(const char* name) {
    for (const char* tag : {"PointLight", "SpotLight", "BoxLight", "AreaLight",
                           "Billboard", "ParticleSystem", "FogArea", "Plane",
                           "StaticObject", "Decal"})
        if (std::strcmp(name, tag) == 0) return true;
    return false;
}

inline const char* RendererXmlBaseName(const char* name) {
    return std::strncmp(name, "Re_", 3) == 0 && IsRendererXmlVisual(name + 3)
        ? name + 3 : name;
}

inline unsigned RendererXmlMask(const tinyxml2::XMLElement* object) {
    const char* tag = object->Value();
    const char* base = RendererXmlBaseName(tag);
    const bool rtOnly = base != tag || std::strcmp(base, "AreaLight") == 0;
    const unsigned mask = SanitizeRendererMask(static_cast<unsigned>(object->IntAttribute(
        "RendererMask", rtOnly ? kRendererMaskRayTraced : kRendererMaskAll)));
    return rtOnly ? mask & kRendererMaskRayTraced : mask;
}

// Redux strips compatibility prefixes in memory before normal loading. Leave
// attributes (including Re_ light tuning) and object identities untouched.
inline void NormalizeRendererXml(tinyxml2::XMLElement* root) {
    if (!root) return;
    const char* base = RendererXmlBaseName(root->Value());
    if (base != root->Value()) {
        const std::string name(base);
        root->SetAttribute("RendererMask", RendererXmlMask(root));
        root->SetName(name.c_str());
    }
    for (auto* child = root->FirstChildElement(); child;) {
        auto* next = child->NextSiblingElement();
        const std::string name(child->Value());
        if (name == "Re_StaticObjects" || name == "Re_Decals") {
            const std::string category = name.substr(3);
            auto* target = root->FirstChildElement(category.c_str());
            if (!target) {
                target = root->GetDocument()->NewElement(category.c_str());
                root->InsertEndChild(target);
            }
            while (auto* object = child->FirstChildElement()) {
                object->SetAttribute("RendererMask", RendererXmlMask(object) & kRendererMaskRayTraced);
                NormalizeRendererXml(object);
                target->InsertEndChild(object);
            }
            root->DeleteChild(child);
        } else {
            NormalizeRendererXml(child);
        }
        child = next;
    }
}

inline void EncodeNormalizedRendererXml(tinyxml2::XMLElement* root) {
    if (!root) return;
    // Snapshot the children: encoding a category can append a sibling.
    std::vector<tinyxml2::XMLElement*> children;
    for (auto* child = root->FirstChildElement(); child; child = child->NextSiblingElement()) children.push_back(child);
    for (auto* child : children) EncodeNormalizedRendererXml(child);
    const std::string name(root->Value());
    if (name == "StaticObjects" || name == "Decals") {
        // Retail instantiates every child of these categories regardless of
        // the child's name. Prefix the category for RT-only entries instead.
        auto* parent = root->Parent() ? root->Parent()->ToElement() : nullptr;
        if (!parent) return;
        for (auto* object = root->FirstChildElement(); object;) {
            auto* next = object->NextSiblingElement();
            if ((RendererXmlMask(object) & kRendererMaskStandard) == 0) {
                const std::string base(RendererXmlBaseName(object->Value()));
                object->SetName(base.c_str());
                const std::string alias = "Re_" + name;
                auto* target = parent->FirstChildElement(alias.c_str());
                if (!target) {
                    target = root->GetDocument()->NewElement(alias.c_str());
                    parent->InsertEndChild(target);
                }
                target->InsertEndChild(object);
            }
            object = next;
        }
    } else if (IsRendererXmlVisual(name.c_str())) {
        const unsigned mask = RendererXmlMask(root);
        if ((mask & kRendererMaskStandard) == 0) {
            root->SetAttribute("RendererMask", mask);
            root->SetName(("Re_" + name).c_str());
        }
    }
}

// Call on the complete map/entity document, including category parents.
inline void EncodeRendererXml(tinyxml2::XMLElement* root) {
    NormalizeRendererXml(root);
    EncodeNormalizedRendererXml(root);
}
}
#endif
