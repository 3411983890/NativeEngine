#include "world.hpp"
#include "../assets/file_path.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace engine::gameplay {
void check(bool condition, const std::string& code, const std::string& message) {
    if (!condition) throw Error(code, message);
}
Json validate(const Json& s, const Json& v, const std::string& path, unsigned depth) {
    check(depth <= 16, "NG-E-STATE-SCHEMA", path + ": nesting limit");
    if (s.contains("oneOf")) {
        unsigned matches = 0; Json out;
        for (const auto& option : s["oneOf"]) try { out = validate(option, v, path, depth + 1); ++matches; } catch (const Error&) {}
        check(matches == 1, "NG-E-STATE-SCHEMA", path + ": expected exactly one variant");
        return out;
    }
    const auto type = s.value("type", "");
    bool ok = type == "object" ? v.is_object() : type == "array" ? v.is_array() :
        type == "boolean" ? v.is_boolean() : type == "string" ? v.is_string() :
        type == "integer" ? v.is_number_integer() : type == "number" ? v.is_number() : false;
    check(ok, "NG-E-STATE-SCHEMA", path + ": expected " + type);
    if (s.contains("enum")) check(std::find(s["enum"].begin(), s["enum"].end(), v) != s["enum"].end(),
                                  "NG-E-STATE-SCHEMA", path + ": invalid enum");
    if (v.is_number()) {
        const double n = v.get<double>();
        check(std::isfinite(n) && n >= s.value("minimum", -1e15) && n <= s.value("maximum", 1e15),
              "NG-E-STATE-SCHEMA", path + ": numeric bounds");
        return v;
    }
    if (v.is_string()) {
        check(v.get_ref<const std::string&>().size() <= s.value("maxLength", 4096u),
              "NG-E-STATE-SCHEMA", path + ": string length");
        return v;
    }
    if (v.is_array()) {
        check(v.size() >= s.value("minItems", 0u) && v.size() <= s.value("maxItems", 256u),
              "NG-E-STATE-SCHEMA", path + ": array length");
        Json out = Json::array();
        for (std::size_t i = 0; i < v.size(); ++i) out.push_back(validate(s.at("items"), v[i], path + "[" + std::to_string(i) + "]", depth + 1));
        return out;
    }
    if (v.is_object()) {
        check(v.size() <= 256, "NG-E-STATE-SCHEMA", path + ": field limit");
        Json out = Json::object();
        const Json props = s.value("properties", Json::object());
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (props.contains(it.key())) out[it.key()] = validate(props[it.key()], it.value(), path + "." + it.key(), depth + 1);
            else {
                check(s.contains("additionalProperties") && s["additionalProperties"].is_object(),
                      "NG-E-STATE-SCHEMA", path + ": unknown field " + it.key());
                out[it.key()] = validate(s["additionalProperties"], it.value(), path + "." + it.key(), depth + 1);
            }
        }
        for (auto it = props.begin(); it != props.end(); ++it)
            if (!out.contains(it.key()) && it.value().contains("default")) out[it.key()] = validate(it.value(), it.value()["default"], path + "." + it.key(), depth + 1);
        for (const auto& required : s.value("required", Json::array()))
            check(out.contains(required.get<std::string>()), "NG-E-STATE-SCHEMA", path + ": required " + required.get<std::string>());
        return out;
    }
    return v;
}
std::string fingerprint(const Json& value) {
    determinism::Hasher h;
    h.text(value.dump());
    return h.hex();
}
Json readJson(const std::string& path) {
    std::ifstream file(engine::assets::nativeFilePath(std::filesystem::u8path(path)), std::ios::binary);
    check(file.good(), "NG-E-IO-GAMEPLAY", "Cannot read " + path);
    file.seekg(0, std::ios::end);
    check(file.tellg() <= 32 * 1024 * 1024, "NG-E-IO-GAMEPLAY", "File exceeds 32 MiB");
    file.seekg(0);
    return Json::parse(file);
}
void atomicSave(const std::string& path, const Json& value) {
    const auto target = std::filesystem::u8path(path);
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path());
    const auto temp = std::filesystem::u8path(path + ".tmp");
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        check(file.good(), "NG-E-SAVE-IO", "Cannot write save");
        const Json envelope = {{"format", "nativegpu.save/1"}, {"checksum", fingerprint(value)}, {"payload", value}};
        file << envelope.dump() << '\n';
        file.flush();
        check(file.good(), "NG-E-SAVE-IO", "Cannot flush save");
    }
#ifdef _WIN32
    check(MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
          "NG-E-SAVE-IO", "Cannot replace save");
#else
    std::filesystem::rename(temp, target);
#endif
}
Json verifiedLoad(const std::string& path) {
    const auto env = readJson(path);
    check(env.at("format") == "nativegpu.save/1" && env.at("checksum") == fingerprint(env.at("payload")),
          "NG-E-SAVE-CHECKSUM", "Save format or checksum mismatch");
    return env.at("payload");
}
void Registry::add(const std::string& name, Json definition) {
    check(!name.empty() && !definitions_.count(name) && definition.at("version").get<unsigned>() > 0,
          "NG-E-COMPONENT-REGISTRY", "Duplicate/invalid component " + name);
    check(definition.at("schema").value("type", "") == "object", "NG-E-COMPONENT-REGISTRY", "Component must be an object");
    definitions_.emplace(name, std::move(definition));
}
void Registry::load(const Json& catalog) {
    for (auto it = catalog.begin(); it != catalog.end(); ++it) add(it.key(), it.value());
}
const Json& Registry::definition(const std::string& name) const {
    auto it = definitions_.find(name);
    check(it != definitions_.end(), "NG-E-COMPONENT-TYPE", "Unknown component " + name);
    return it->second;
}
Json Registry::normalize(const std::string& name, const Json& value) const { return validate(definition(name).at("schema"), value, name); }
Json Registry::catalog() const { return definitions_; }
Handle World::create(const std::string& id) {
    check(!id.empty() && id.size() <= 128 && !ids_.count(id) && !retired_.count(id) && ids_.size() < 10000, "NG-E-ENTITY-ID", "Invalid/duplicate entity " + id);
    unsigned index = 0;
    while (index < slots_.size() && slots_[index].alive) ++index;
    if (index == slots_.size()) slots_.emplace_back();
    auto& e = slots_[index];
    e.id = id; e.alive = true; e.components.clear(); ids_[id] = index;
    return {index, e.generation};
}
bool World::valid(Handle h) const { return h.index < slots_.size() && slots_[h.index].alive && slots_[h.index].generation == h.generation; }
Handle World::find(const std::string& id) const {
    auto it = ids_.find(id);
    return it == ids_.end() ? Handle{} : Handle{it->second, slots_[it->second].generation};
}
const EntityRecord& World::entity(Handle h) const { check(valid(h), "NG-E-ENTITY-STALE", "Stale entity handle"); return slots_[h.index]; }
EntityRecord& World::entity(Handle h) { check(valid(h), "NG-E-ENTITY-STALE", "Stale entity handle"); return slots_[h.index]; }
void World::destroy(Handle h) {
    auto& e = entity(h); retired_.insert(e.id); ids_.erase(e.id); e.alive = false; e.components.clear();
    check(e.generation != ~0u, "NG-E-ENTITY-GENERATION", "Generation exhausted"); ++e.generation;
}
void World::set(Handle h, const std::string& c, const Json& value) { entity(h).components[c] = registry.normalize(c, value); }
void World::remove(Handle h, const std::string& c) { entity(h).components.erase(c); }
const Json* World::get(Handle h, const std::string& c) const {
    const auto& components = entity(h).components;
    auto it = components.find(c); return it == components.end() ? nullptr : &it->second;
}
std::vector<Handle> World::query(const std::string& c) const {
    std::vector<Handle> out;
    for (const auto& [id, index] : ids_) {
        (void)id;
        const auto& e = slots_[index];
        if (c.empty() || e.components.count(c)) out.push_back({index, e.generation});
    }
    return out;
}
Json World::view() const {
    Json result = Json::object();
    for (auto h : query()) result[entity(h).id] = entity(h).components;
    return result;
}
Json World::snapshot() const {
    Json slots = Json::array();
    for (const auto& e : slots_) slots.push_back({{"id", e.id}, {"generation", e.generation}, {"alive", e.alive}, {"components", e.components}});
    return {{"format", 1}, {"registry", fingerprint(registry.catalog())}, {"slots", slots}, {"retired",retired_}};
}
void World::restore(const Json& state) {
    check(state.at("format") == 1 && state.at("registry") == fingerprint(registry.catalog()), "NG-E-SNAPSHOT-VERSION", "World registry changed");
    World candidate; candidate.registry = registry;
    candidate.retired_=state.at("retired").get<std::set<std::string>>();
    for(const auto& id:candidate.retired_)check(!id.empty()&&id.size()<=128,"NG-E-SNAPSHOT-STATE","Invalid retired identity");
    check(state.at("slots").is_array() && state.at("slots").size() <= 10000, "NG-E-SNAPSHOT-STATE", "Too many slots");
    for (const auto& s : state.at("slots")) {
        EntityRecord e; e.id = s.at("id"); e.generation = s.at("generation"); e.alive = s.at("alive");
        check(e.generation > 0 && !e.id.empty() && e.id.size()<=128 && s.at("components").is_object(), "NG-E-SNAPSHOT-STATE", "Invalid generation/entity");
        if (e.alive) {
            check(!candidate.retired_.count(e.id) && !candidate.ids_.count(e.id), "NG-E-SNAPSHOT-STATE", "Duplicate entity");
            for (auto it = s.at("components").begin(); it != s.at("components").end(); ++it)
                e.components[it.key()] = registry.normalize(it.key(), it.value());
            candidate.ids_[e.id] = static_cast<unsigned>(candidate.slots_.size());
        }
        candidate.slots_.push_back(std::move(e));
    }
    *this = std::move(candidate);
}
void Scheduler::add(System system) {
    check(!system.name.empty() && !systems_.count(system.name), "NG-E-SYSTEM-DUPLICATE", "Duplicate system");
    systems_.emplace(system.name, std::move(system)); order_.clear();
}
void Scheduler::compile() {
    order_.clear(); std::set<std::string> done;
    for (const auto& [name, s] : systems_) for (const auto& dep : s.after) {
        auto it = systems_.find(dep);
        check(it != systems_.end() && it->second.phase <= s.phase, "NG-E-SYSTEM-DEPENDENCY", name + ": invalid dependency " + dep);
    }
    while (done.size() < systems_.size()) {
        bool progress = false;
        for (int phase = 0; phase <= 5 && !progress; ++phase) {
            bool pending = false;
            for (const auto& [name, s] : systems_) {
                if (static_cast<int>(s.phase) != phase || done.count(name)) continue;
                pending = true;
                if (std::all_of(s.after.begin(), s.after.end(), [&](const auto& d) { return done.count(d); })) {
                    order_.push_back(name); done.insert(name); progress = true; break;
                }
            }
            if (pending) break;
        }
        check(progress, "NG-E-SYSTEM-CYCLE", "System dependency cycle");
    }
}
void Scheduler::run() { if (order_.size() != systems_.size()) compile(); for (const auto& name : order_) if (systems_.at(name).run) systems_.at(name).run(); }
std::vector<std::string> Scheduler::order() const { return order_; }
}
