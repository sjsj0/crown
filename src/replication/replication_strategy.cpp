#include "replication_strategy.h"
#include "chain.pb.h"   // gives the compiler the full VersionQueryResponse definition

chain::VersionQueryResponse
ReplicationStrategy::handle_version_query(const chain::VersionQueryRequest&, Node&) {
    throw runtime_error("handle_version_query not implemented for this strategy");
}
