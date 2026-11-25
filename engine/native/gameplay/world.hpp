#pragma once

#include "../../../third_party/json.hpp"
#include "../determinism.hpp"
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <set>
#include <vector>

namespace engine::gameplay {
using Json = nlohmann::json;
struct Error : std::runtime_error {
    std::string code;
    Error(std::string c, const std::string& message) : std::runtime_error(message), code(std::move(c)) {}
};
void check(bool condition, const std::string& code, const std::string& message);
// Shared schema subset: object/array/scalars, defaults, required, bounds and enum.
Json validate(const Json& schema, const Json& value, const std::string& path = "value", unsigned depth = 0);
std::string fingerprint(const Json& value);
Json readJson(const std::string& path);
void atomicSave(const std::string& path, const Json& value);
Json verifiedLoad(const std::string& path);

struct Handle {
    unsigned index = ~0u, generation = 0;
    bool operator==(const Handle& b) const { return index == b.index && generation == b.generation; }
};
struct EntityRecord {
    std::string id;
    unsigned generation = 1;
    bool alive = false;
    std::map<std::string, Json> components;
};
class Registry {
public:
    void add(const std::string& name, Json definition);
    void load(const Json& catalog);
    const Json& definition(const std::string& name) const;
    Json normalize(const std::string& name, const Json& value) const;
    Json catalog() const;
private:
    std::map<std::string, Json> definitions_;
};
class World {
public:
    Registry registry;
    Handle create(const std::string& id);
    bool valid(Handle handle) const;
    Handle find(const std::string& id) const;
    const EntityRecord& entity(Handle handle) const;
    EntityRecord& entity(Handle handle);
    void destroy(Handle handle);
    void set(Handle handle, const std::string& component, const Json& value);
    void remove(Handle handle, const std::string& component);
    const Json* get(Handle handle, const std::string& component) const;
    std::vector<Handle> query(const std::string& component = "") const;
    Json view() const;
    Json snapshot() const;
    void restore(const Json& state);
private:
    std::vector<EntityRecord> slots_;
    std::map<std::string, unsigned> ids_;
    std::set<std::string> retired_;
};
enum class Phase { Input, Events, Behaviors, Simulation, Collect, Commit };
struct System {
    std::string name;
    Phase phase = Phase::Simulation;
    std::vector<std::string> after;
    std::function<void()> run;
};
class Scheduler {
public:
    void add(System system);
    void compile();
    void run();
    std::vector<std::string> order() const;
private:
    std::map<std::string, System> systems_;
    std::vector<std::string> order_;
};
}
