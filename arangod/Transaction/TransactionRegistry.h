////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Kaveh Vahedipour
/// @author Max Neunhoeffer
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_TRANSACTION_TRANSACTION_REGISTRY_H
#define ARANGOD_TRANSACTION_TRANSACTION_REGISTRY_H 1

#include "types.h"

#include "Basics/Common.h"
#include "Basics/Mutex.h"

struct TRI_vocbase_t;

namespace arangodb {
namespace transaction {
class Methods;

class TransactionRegistry {
 public:
  TransactionRegistry() {}

  ~TransactionRegistry();

  /// @brief insert, this inserts the transaction <transaction> for the vocbase <vocbase>
  /// and the id <id> into the registry. It is in error if there is already
  /// a transaction for this <vocbase> and <id> combination and an exception will
  /// be thrown in that case. The time to live <ttl> is in seconds and the
  /// transaction will be deleted if it is not opened for that amount of time.
  void insert(TransactionId id, Methods* transaction, double ttl = 60.0);

  /// @brief Lease and open a transaction
  Methods* open(TRI_vocbase_t* vocbase, TransactionId id);

  /// @brief Return a leased open transaction 
  void close(TRI_vocbase_t* vocbase, TransactionId id, double ttl = -1.0);

  /// @brief Return and commit an open transaction
  void closeCommit(TRI_vocbase_t* vocbase, TransactionId id, double ttl = -1.0);

  /// @brief Return and abort an open transaction
  void closeAbort(TRI_vocbase_t* vocbase, TransactionId id, double ttl = -1.0);

  /// @brief destroy, this removes the entry from the registry and calls
  /// delete on the Transaction*. It is allowed to call this regardless of whether
  /// the transaction is open or closed. No check is performed that this call comes
  /// from the same thread that has opened it! Note that if the transaction is
  /// "open", then this will set the "killed" flag in the transaction and do not
  /// more.
  void destroy(std::string const& vocbase, TransactionId id, int errorCode);

  void destroy(TRI_vocbase_t* vocbase, TransactionId id, int errorCode);

  /// @brief expireTransactions, this deletes all expired transactions from the registry
  void expireTransactions();

  /// @brief return number of registered transactions
  size_t numberRegisteredTransactions();

  /// @brief for shutdown, we need to shut down all transactions:
  void destroyAll();

 private:
  /// @brief a struct for all information regarding one transaction in the registry
  struct TransactionInfo {
    TRI_vocbase_t* _vocbase;  // the vocbase
    TransactionId _id;        // id of the transaction
    Methods* _transaction;    // the actual transaction pointer
    bool _isOpen;  // flag indicating whether or not the transaction is in use
    double _timeToLive;       // in seconds
    double _expires;          // UNIX UTC timestamp of expiration
  };

  /// @brief _transactions, the actual map of maps for the registry
  std::unordered_map<std::string, std::unordered_map<TransactionId, TransactionInfo*>>
      _transactions;

  /// @brief _lock, the read/write lock for access
  arangodb::Mutex _lock;
};

}  // namespace arangodb::transaction
}  // namespace arangodb

#endif