#ifndef KRPC_ZK_C_API_H
#define KRPC_ZK_C_API_H

// ZooKeeper 3.5+ (Ubuntu 24.04 ships 3.9) only declares the synchronous C API
// when THREADED is defined. Link against libzookeeper_mt. Ubuntu 20.04's 3.4
// headers still compile with this define.
#ifndef THREADED
#define THREADED
#endif

#include <zookeeper/zookeeper.h>

#endif
