////////////////////////////////////////////////////////////////////////////////
/// @brief result set
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2011 triagens GmbH, Cologne, Germany
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
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
/// @author Copyright 2011, triagens GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_DURHAM_VOC_BASE_RESULT_SET_H
#define TRIAGENS_DURHAM_VOC_BASE_RESULT_SET_H 1

#include "VocBase/vocbase.h"

#include "BasicsC/json.h"
#include "BasicsC/locks.h"
#include "ShapedJson/shaped-json.h"
#include "VocBase/datafile.h"

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// --SECTION--                                              forward declarations
// -----------------------------------------------------------------------------

struct TRI_doc_collection_s;
struct TRI_doc_mptr_s;
struct TRI_barrier_list_s;
struct TRI_barrier_s;

// -----------------------------------------------------------------------------
// --SECTION--                                                      public types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief an identifier for result sets
////////////////////////////////////////////////////////////////////////////////

typedef TRI_voc_tick_t TRI_rs_id_t;

////////////////////////////////////////////////////////////////////////////////
/// @brief information about the execution
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_rs_info_s {
  char* _cursor;

  TRI_voc_size_t _scannedIndexEntries;
  TRI_voc_size_t _scannedDocuments;
  TRI_voc_size_t _matchedDocuments;

  double _runtime;
}
TRI_rs_info_t;

////////////////////////////////////////////////////////////////////////////////
/// @brief result set entry
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_rs_entry_s {
  TRI_shaped_json_t _document;
  TRI_json_t _augmented;
  TRI_df_marker_type_e _type;

  TRI_voc_did_t _did;
  TRI_voc_rid_t _rid;

  TRI_voc_cid_t _fromCid;
  TRI_voc_did_t _fromDid;

  TRI_voc_cid_t _toCid;
  TRI_voc_did_t _toDid;
}
TRI_rs_entry_t;

////////////////////////////////////////////////////////////////////////////////
/// @brief a result set
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_result_set_s {
  TRI_rs_id_t _id;
  TRI_rs_info_t _info;

  struct TRI_barrier_s* _barrier;

  char* _error;

  bool (*hasNext) (struct TRI_result_set_s*);
  TRI_rs_entry_t const* (*next) (struct TRI_result_set_s*);
  TRI_voc_size_t (*count) (struct TRI_result_set_s*, bool current);

  void (*free) (struct TRI_result_set_s*);
}
TRI_result_set_t;

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a single result set
////////////////////////////////////////////////////////////////////////////////

TRI_result_set_t* TRI_CreateRSSingle (struct TRI_doc_collection_s* collection,
                                      struct TRI_barrier_s* containerElement,
                                      struct TRI_doc_mptr_s const* header,
                                      TRI_voc_size_t total);

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a full result set
////////////////////////////////////////////////////////////////////////////////

TRI_result_set_t* TRI_CreateRSVector (struct TRI_doc_collection_s* collection,
                                      struct TRI_barrier_s* containerElement,
                                      struct TRI_doc_mptr_s const** header,
                                      TRI_json_t const* augmented,
                                      TRI_voc_size_t length,
                                      TRI_voc_size_t total);

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif

#endif

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|// --SECTION--\\|/// @\\}\\)"
// End:
