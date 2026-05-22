//
//  TDB.h
//  TDB — Objective-C framework umbrella header.
//
//  This framework wraps the TDB embedded SQL database (libtdb) for use from
//  Objective-C and Swift on Apple platforms (macOS, iOS, tvOS, watchOS).
//
//  Design notes:
//    * The public surface follows the same conventions used by Apple's
//      Network.framework: opaque C types vended via NSObject pointers with a
//      protocol marker (`OS_tdb_*`), paired with high-level NS classes.
//    * All blocking API has a sync overload (returning NSError**) and an
//      async overload taking a dispatch_queue_t plus completion handler.
//    * Value bridging is built in: NSString <-> STRING, NSNumber <-> INT64 /
//      FLOAT64 / BOOL, NSData <-> BLOB / VARBINARY, NSArray <-> ARRAY,
//      NSDictionary <-> COMPOSITE (with type name as a sentinel key),
//      NSUUID <-> UUID, NSDate <-> TIMESTAMP. Round-trip is lossless when
//      the source already has a canonical representation.
//    * Encryption uses the in-tree AES-256-GCM primitive plus
//      Security.framework: SecRandomCopyBytes for nonces, Keychain Services
//      via TDBKeychain for at-rest key storage.
//
//  Apache 2.0 — Copyright 2026 Rick Holland.
//

#ifndef TDB_OBJC_UMBRELLA_H
#define TDB_OBJC_UMBRELLA_H

#import <Foundation/Foundation.h>

#import "TDBTypes.h"
#import "TDBValue.h"
#import "TDBResultSet.h"
#import "TDBDatabase.h"
#import "TDBCrypto.h"
#import "TDBKeychain.h"

#endif /* TDB_OBJC_UMBRELLA_H */
