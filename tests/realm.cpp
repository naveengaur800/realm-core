////////////////////////////////////////////////////////////////////////////
//
// Copyright 2016 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include "catch.hpp"

#include "util/event_loop.hpp"
#include "util/test_file.hpp"

#include "binding_context.hpp"
#include "object_schema.hpp"
#include "object_store.hpp"
#include "property.hpp"
#include "schema.hpp"

#include <realm/group.hpp>

#include <unistd.h>

using namespace realm;

TEST_CASE("SharedRealm: get_shared_realm()") {
    TestFile config;
    config.schema_version = 1;
    config.schema = Schema{
        {"object", {
            {"value", PropertyType::Int, "", "", false, false, false}
        }},
    };

    SECTION("should return the same instance when caching is enabled") {
        auto realm1 = Realm::get_shared_realm(config);
        auto realm2 = Realm::get_shared_realm(config);
        REQUIRE(realm1.get() == realm2.get());
    }

    SECTION("should return different instances when caching is disabled") {
        config.cache = false;
        auto realm1 = Realm::get_shared_realm(config);
        auto realm2 = Realm::get_shared_realm(config);
        REQUIRE(realm1.get() != realm2.get());
    }

    SECTION("should validate that the config is sensible") {
        SECTION("bad encryption key") {
            config.encryption_key = std::vector<char>(2, 0);
            REQUIRE_THROWS(Realm::get_shared_realm(config));
        }

        SECTION("schema without schema version") {
            config.schema_version = ObjectStore::NotVersioned;
            REQUIRE_THROWS(Realm::get_shared_realm(config));
        }

        SECTION("migration function for read-only") {
            config.schema_mode = SchemaMode::ReadOnly;
            config.migration_function = [](auto, auto, auto) { };
            REQUIRE_THROWS(Realm::get_shared_realm(config));
        }

        SECTION("migration function for additive-only") {
            config.schema_mode = SchemaMode::Additive;
            config.migration_function = [](auto, auto, auto) { };
            REQUIRE_THROWS(Realm::get_shared_realm(config));
        }
    }

    SECTION("should reject mismatched config") {
        config.cache = false;

        SECTION("schema version") {
            auto realm = Realm::get_shared_realm(config);
            config.schema_version = 2;
            REQUIRE_THROWS(Realm::get_shared_realm(config));

            config.schema = util::none;
            config.schema_version = ObjectStore::NotVersioned;
            REQUIRE_NOTHROW(Realm::get_shared_realm(config));
        }

        SECTION("schema mode") {
            auto realm = Realm::get_shared_realm(config);
            config.schema_mode = SchemaMode::Manual;
            REQUIRE_THROWS(Realm::get_shared_realm(config));
        }

        SECTION("durability") {
            auto realm = Realm::get_shared_realm(config);
            config.in_memory = true;
            REQUIRE_THROWS(Realm::get_shared_realm(config));
        }

        SECTION("schema") {
            auto realm = Realm::get_shared_realm(config);
            config.schema = Schema{
                {"object", {
                    {"value", PropertyType::Int, "", "", false, false, false},
                    {"value2", PropertyType::Int, "", "", false, false, false}
                }},
            };
            REQUIRE_THROWS(Realm::get_shared_realm(config));
        }
    }

    SECTION("should apply the schema if one is supplied") {
        Realm::get_shared_realm(config);

        {
            Group g(config.path);
            auto table = ObjectStore::table_for_object_type(g, "object");
            REQUIRE(table);
            REQUIRE(table->get_column_count() == 1);
            REQUIRE(table->get_column_name(0) == "value");
        }

        config.schema_version = 2;
        config.schema = Schema{
            {"object", {
                {"value", PropertyType::Int, "", "", false, false, false},
                {"value2", PropertyType::Int, "", "", false, false, false}
            }},
        };
        bool migration_called = false;
        config.migration_function = [&](SharedRealm old_realm, SharedRealm new_realm, Schema&) {
            migration_called = true;
            REQUIRE(ObjectStore::table_for_object_type(old_realm->read_group(), "object")->get_column_count() == 1);
            REQUIRE(ObjectStore::table_for_object_type(new_realm->read_group(), "object")->get_column_count() == 2);
        };
        Realm::get_shared_realm(config);
        REQUIRE(migration_called);
    }

    SECTION("should properly roll back from migration errors") {
        Realm::get_shared_realm(config);

        config.schema_version = 2;
        config.schema = Schema{
            {"object", {
                {"value", PropertyType::Int, "", "", false, false, false},
                {"value2", PropertyType::Int, "", "", false, false, false}
            }},
        };
        bool migration_called = false;
        config.migration_function = [&](SharedRealm old_realm, SharedRealm new_realm, Schema&) {
            REQUIRE(ObjectStore::table_for_object_type(old_realm->read_group(), "object")->get_column_count() == 1);
            REQUIRE(ObjectStore::table_for_object_type(new_realm->read_group(), "object")->get_column_count() == 2);
            if (!migration_called) {
                migration_called = true;
                throw "error";
            }
        };
        REQUIRE_THROWS_WITH(Realm::get_shared_realm(config), "error");
        REQUIRE(migration_called);
        REQUIRE_NOTHROW(Realm::get_shared_realm(config));
    }

    SECTION("should read the schema from the file if none is supplied") {
        Realm::get_shared_realm(config);

        config.schema = util::none;
        auto realm = Realm::get_shared_realm(config);
        REQUIRE(realm->schema().size() == 1);
        auto it = realm->schema().find("object");
        REQUIRE(it != realm->schema().end());
        REQUIRE(it->persisted_properties.size() == 1);
        REQUIRE(it->persisted_properties[0].name == "value");
        REQUIRE(it->persisted_properties[0].table_column == 0);
    }

    SECTION("should populate the table columns in the schema when opening as read-only") {
        Realm::get_shared_realm(config);

        config.schema_mode = SchemaMode::ReadOnly;
        auto realm = Realm::get_shared_realm(config);
        auto it = realm->schema().find("object");
        REQUIRE(it != realm->schema().end());
        REQUIRE(it->persisted_properties.size() == 1);
        REQUIRE(it->persisted_properties[0].name == "value");
        REQUIRE(it->persisted_properties[0].table_column == 0);
    }

    SECTION("should throw when creating the notification pipe fails") {
        util::try_make_dir(config.path + ".note");
        REQUIRE_THROWS(Realm::get_shared_realm(config));
        util::remove_dir(config.path + ".note");
    }

    SECTION("should get different instances on different threads") {
        auto realm1 = Realm::get_shared_realm(config);
        std::thread([&]{
            auto realm2 = Realm::get_shared_realm(config);
            REQUIRE(realm1 != realm2);
        }).join();
    }

    SECTION("should detect use of Realm on incorrect thread") {
        auto realm = Realm::get_shared_realm(config);
        std::thread([&]{
            REQUIRE_THROWS_AS(realm->verify_thread(), IncorrectThreadException);
        }).join();
    }

    SECTION("should get different instances for different explicit execuction contexts") {
        config.execution_context = 0;
        auto realm1 = Realm::get_shared_realm(config);
        config.execution_context = 1;
        auto realm2 = Realm::get_shared_realm(config);
        REQUIRE(realm1 != realm2);

        config.execution_context = util::none;
        auto realm3 = Realm::get_shared_realm(config);
        REQUIRE(realm1 != realm3);
        REQUIRE(realm2 != realm3);
    }

    SECTION("can use Realm with explicit execution context on different thread") {
        config.execution_context = 1;
        auto realm = Realm::get_shared_realm(config);
        std::thread([&]{
            REQUIRE_NOTHROW(realm->verify_thread());
        }).join();
    }

    SECTION("should get same instance for same explicit execution context on different thread") {
        config.execution_context = 1;
        auto realm1 = Realm::get_shared_realm(config);
        std::thread([&]{
            auto realm2 = Realm::get_shared_realm(config);
            REQUIRE(realm1 == realm2);
        }).join();
    }
}

TEST_CASE("SharedRealm: notifications") {
    if (!util::EventLoop::has_implementation())
        return;

    TestFile config;
    config.cache = false;
    config.schema_version = 0;
    config.schema = Schema{
        {"object", {
            {"value", PropertyType::Int, "", "", false, false, false}
        }},
    };

    struct Context : BindingContext {
        size_t* change_count;
        Context(size_t* out) : change_count(out) { }

        void did_change(std::vector<ObserverState> const&, std::vector<void*> const&, bool) override
        {
            ++*change_count;
        }
    };

    size_t change_count = 0;
    auto realm = Realm::get_shared_realm(config);
    realm->m_binding_context.reset(new Context{&change_count});

    SECTION("local notifications are sent synchronously") {
        realm->begin_transaction();
        REQUIRE(change_count == 0);
        realm->commit_transaction();
        REQUIRE(change_count == 1);
    }

    SECTION("remote notifications are sent asynchronously") {
        auto r2 = Realm::get_shared_realm(config);
        r2->begin_transaction();
        r2->commit_transaction();
        REQUIRE(change_count == 0);
        util::EventLoop::main().run_until([&]{ return change_count > 0; });
        REQUIRE(change_count == 1);
    }
}

TEST_CASE("SharedRealm: closed realm") {
    TestFile config;
    config.schema_version = 1;
    config.schema = Schema{
        {"object", {
            {"value", PropertyType::Int, "", "", false, false, false}
        }},
    };

    auto realm = Realm::get_shared_realm(config);
    realm->close();

    REQUIRE(realm->is_closed());

    REQUIRE_THROWS_AS(realm->read_group(), ClosedRealmException);
    REQUIRE_THROWS_AS(realm->begin_transaction(), ClosedRealmException);
    REQUIRE(!realm->is_in_transaction());
    REQUIRE_THROWS_AS(realm->commit_transaction(), InvalidTransactionException);
    REQUIRE_THROWS_AS(realm->cancel_transaction(), InvalidTransactionException);

    REQUIRE_THROWS_AS(realm->refresh(), ClosedRealmException);
    REQUIRE_THROWS_AS(realm->invalidate(), ClosedRealmException);
    REQUIRE_THROWS_AS(realm->compact(), ClosedRealmException);
}

TEST_CASE("ShareRealm: in-memory mode from buffer") {
    TestFile config;
    config.schema_version = 1;
    config.schema = Schema{
        {"object", {
            {"value", PropertyType::Int, "", "", false, false, false}
        }},
    };
    
    SECTION("Save and open Realm from in-memory buffer") {
        // Write in-memory copy of Realm to a buffer
        auto realm = Realm::get_shared_realm(config);
        OwnedBinaryData realm_buffer = realm->write_copy_to_mem();
        
        // Open the buffer as a new (read-only in-memory) Realm
        realm::Realm::Config config2;
        config2.schema_mode = SchemaMode::ReadOnly;
        config2.realm_data = realm_buffer.get();
        
        auto realm2 = Realm::get_shared_realm(config2);
        
        // Verify that it can read the schema and that it is the same
        REQUIRE(realm->schema().size() == 1);
        auto it = realm->schema().find("object");
        REQUIRE(it != realm->schema().end());
        REQUIRE(it->persisted_properties.size() == 1);
        REQUIRE(it->persisted_properties[0].name == "value");
        REQUIRE(it->persisted_properties[0].table_column == 0);
    }
}
