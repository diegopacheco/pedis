#include "redis/commands/del.hh"
#include "redis/commands/unexpected.hh"
#include "seastar/core/shared_ptr.hh"
#include "redis/reply_builder.hh"
#include "redis/request.hh"
#include "redis/redis_mutation.hh"
#include "redis/reply.hh"
#include "db/system_keyspace.hh"
#include "types.hh"
#include "service/storage_proxy.hh"
#include "service/client_state.hh"
#include "mutation.hh"
#include "timeout_config.hh"
#include "redis/prefetcher.hh"
namespace service {
class storage_proxy;
}
namespace redis {

namespace commands {

shared_ptr<abstract_command> del::prepare(service::storage_proxy& proxy, request&& req)
{
    if (req._args_count < 1) {
        return unexpected::prepare(std::move(req._command), std::move(bytes {msg_syntax_err}));
    }
    std::vector<schema_ptr> schemas { simple_objects_schema(proxy), lists_schema(proxy), sets_schema(proxy), maps_schema(proxy) };
    return seastar::make_shared<del> (std::move(req._command), std::move(schemas), std::move(req._args[0]));
}

future<reply> del::execute(service::storage_proxy& proxy, db::consistency_level cl, db::timeout_clock::time_point now, const timeout_config& tc, service::client_state& cs)
{
    auto timeout = now + tc.write_timeout;
    auto remove_if_exists = [this, timeout, cl, &proxy, &tc, &cs] (const schema_ptr schema) {
        return exists(proxy, schema, _key, cl, timeout, cs).then ([this, &proxy, timeout, cl, &cs, schema] (auto exists) {
            if (exists) {
                return redis::write_mutation(proxy, redis::make_dead(schema, _key), cl, timeout, cs).then_wrapped([this] (auto f) {
                    try {
                        f.get();
                    } catch (...) {
                        return make_ready_future<bool>(false);
                    }
                    return make_ready_future<bool>(true);
                });
            }
            return make_ready_future<bool>(false);
        });
    };
    auto mapper = make_lw_shared<decltype(remove_if_exists)>(std::move(remove_if_exists));
    return map_reduce(_schemas.begin(), _schemas.end(), *mapper, false, std::bit_or<bool> ()).then([mapper = std::move(mapper)] (auto result) {
        if (result) {
            return reply_builder::build<ok_tag>();
        }
        return reply_builder::build<error_tag>();
    });
}
}
}
