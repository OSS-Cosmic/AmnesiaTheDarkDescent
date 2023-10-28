#include "absl/container/inlined_vector.h"
#include "engine/Event.h"
#include "math/MathTypes.h"
#include "resources/StringLiteral.h"
#include "resources/rapidXML/rapidxml.hpp"
#include <span>
#include <string_view>
#include <variant>

namespace hpl::config {

    class SettingContext;
    struct ICVar {};

    class ISetting {
    public:
        virtual void Configure(SettingContext& registry) = 0;
    };

    template<typename V>
    class CVarValue : public ICVar{
    public:
        void Set(V value) {
            m_value = value;
        }

        V Get() const {
            return m_value;
        }

    private:
        V m_value;
    };

    template<typename V>
    class CCVar final : public CVarValue<V> {
    public:
        CCVar(const char* name, V defaultValue = V()): 
            m_name(name),
            m_defaultValue(defaultValue) {
        }

        hpl::Event<V&>& OnChanged() {
            return m_changed;
        }

        const char* GetName() const {
            return m_name;
        }
    private:
        hpl::Event<V&> m_changed;
        const char* m_name;
        V m_defaultValue;
    };


    class SettingContext {
    public:

        SettingContext(): 
            m_root(CVarRecordGroup()) {
            m_activeGroup.push(&m_root);
        }

        SettingContext(const SettingContext&) = delete;
        SettingContext(SettingContext&&) = delete;
        void operator=(const SettingContext&) = delete;
        SettingContext& operator=(SettingContext&&) = delete;

      
        struct CVarRecord {
            enum CVarPTypes: uint8_t {
                Int,
                Float
                // ObjectType TODO: need to add reflection env
            };
            CVarPTypes m_type;
            union {
                struct {
                    const char* m_name;
                    ICVar* m_cvar;
                } m_field;
                struct {
                    const char* m_name;
                    uint8_t m_version;
                } m_group;
            };
        };

        struct CVarRecordGroup {
            CVarRecordGroup(SettingContext& context) {
            }

            CVarRecordGroup(): 
                m_name(nullptr), 
                m_version(0) {
            }
            CVarRecordGroup(const CVarRecordGroup&) = delete;
            CVarRecordGroup(CVarRecordGroup&&) = default;

            void operator=(const CVarRecordGroup&) = delete;
            CVarRecordGroup& operator=(CVarRecordGroup&&) = default;

            const std::vector<CVarRecord>& Records() const { return m_records; }
            const std::vector<CVarRecordGroup*>& Groups() const { return m_groups; }
            inline const char* Name() const { return m_name; }
            inline uint8_t Version() const { return m_version;}
        private:
            const char* m_name;
            uint8_t m_version;
            std::vector<CVarRecord> m_records;
            std::vector<CVarRecordGroup*> m_groups;
            std::function<void(CVarRecordGroup& group)> m_converter = nullptr;

            friend class SettingContext;
        };

        SettingContext& Version(uint8_t version) {
            auto& activeGroup = m_activeGroup.top();
            activeGroup->m_version = version;
            return *this;
        }
        SettingContext& Version(uint8_t version, std::function<void(CVarRecordGroup& group)> converter) {
            auto& activeGroup = m_activeGroup.top();
            activeGroup->m_version = version;
            activeGroup->m_converter = converter;
            return *this;
        }

        template<typename V>
        constexpr SettingContext& AddCVars(CCVar<V>& cvar) {
            if constexpr (std::is_same<V, int>()) {
                int value = cvar.Get();
                auto& record = m_activeGroup.top()->m_records.emplace_back();
                record.m_type = CVarRecord::Int;
                record.m_field.m_name = cvar.GetName();
                record.m_field.m_cvar = &cvar;
            }
            return *this;
        }

        SettingContext& EndGroup() {
            m_activeGroup.pop();
            return *this;
        }

        SettingContext& BeginGroup(const char*  group) {
            auto& newGroup = m_groups.emplace_back(std::make_unique<CVarRecordGroup>());
            m_activeGroup.top()->m_groups.push_back(newGroup.get());
            m_activeGroup.emplace(newGroup.get());
            newGroup->m_name = group;
            return *this;
        }

        const CVarRecordGroup& Root() const {
            return m_root;
        }

    private:
        std::stack<CVarRecordGroup*> m_activeGroup;
        std::vector<std::unique_ptr<ICVar>> m_dummyVars; // TODO: use pool allocator
        std::vector<std::unique_ptr<CVarRecordGroup>> m_groups; // TODO: use pool allocator
        CVarRecordGroup m_root;
    };

    inline void Serialize(rapidxml::xml_document<char*>& m_document, SettingContext& setting) {

    }


    class ScreenConfig: public ISetting {
    public:
        CCVar<int> m_screenWidth = {("Width")};
        CCVar<int> m_screenHeight = {("Height")};
        CCVar<int> m_display = {("Display")};
        CCVar<int> m_isFullscreen = {("FullScreen")};
        CCVar<int> m_vSync = {("Vsync")};
        CCVar<int> m_adaptiveVSync = {("AdaptiveVsync")};

        virtual void Configure(SettingContext& registry) override {
            registry.BeginGroup("Screen")
                .Version(0)
                .AddCVars(m_screenWidth)
                .AddCVars(m_screenHeight)
                .AddCVars(m_display)
                .AddCVars(m_isFullscreen)
                .AddCVars(m_vSync)
                .AddCVars(m_adaptiveVSync)
            .EndGroup();
        }
    };


    void test() {
        ScreenConfig m_config;

        hpl::Event<int&>::Handler onScreenWidthChanged([](int& value) {

        });
        onScreenWidthChanged.Connect(m_config.m_screenWidth.OnChanged());
        m_config.m_screenWidth.Set(100);
    }
    class CUserSettings: public ISetting {
    public:
        ScreenConfig m_screenConfig;

     
    };
};