////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "OptimizerRules.h"
#include "Aql/CollectNode.h"
#include "Aql/CollectOptions.h"
#include "Aql/Collection.h"
#include "Aql/DocumentProducingNode.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Function.h"
#include "Aql/IndexNode.h"
#include "Aql/ModificationNodes.h"
#include "Aql/Optimizer.h"
#include "Aql/Query.h"
#include "Aql/ShortestPathNode.h"
#include "Aql/SortCondition.h"
#include "Aql/SortNode.h"
#include "Aql/TraversalConditionFinder.h"
#include "Aql/TraversalNode.h"
#include "Aql/Variable.h"
#include "Aql/types.h"
#include "Basics/AttributeNameParser.h"
#include "Basics/SmallVector.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringBuffer.h"
#include "Cluster/ClusterInfo.h"
#include "Geo/GeoParams.h"
#include "GeoIndex/Index.h"
#include "Graph/TraverserOptions.h"
#include "Indexes/Index.h"
#include "Transaction/Methods.h"
#include "VocBase/Methods/Collections.h"

using namespace arangodb;
using namespace arangodb::aql;
using EN = arangodb::aql::ExecutionNode;

namespace {

//NEAR(coll, 0 /*lat*/, 0 /*lon*/[, 10 /*limit*/])
struct nearOrWithinParams{
  std::string collection;
  AstNode* latitude = nullptr;
  AstNode* longitude = nullptr;
  AstNode* limit = nullptr;
  AstNode* radius = nullptr;
  AstNode* distanceName = nullptr;

  nearOrWithinParams(AstNode const* node, bool isNear){
    TRI_ASSERT(node->type == AstNodeType::NODE_TYPE_FCALL);
    AstNode* arr = node->getMember(0);
    TRI_ASSERT(arr->type == AstNodeType::NODE_TYPE_ARRAY);
    collection = arr->getMember(0)->getString();
    latitude = arr->getMember(1);
    longitude = arr->getMember(2);
    if(arr->numMembers() > 4){
      distanceName = arr->getMember(4);
    } else {
      distanceName = nullptr;
    }

    if(isNear){
      limit = arr->getMember(3);
    } else {
      radius = arr->getMember(3);
    }
  }
};

//FULLTEXT(collection, "attribute", "search", 100 /*limit*/[, "distance name"])
struct fulltextParams{
  std::string collection;
  std::string attribute;
  AstNode* limit = nullptr;

  fulltextParams(AstNode const* node){
    TRI_ASSERT(node->type == AstNodeType::NODE_TYPE_FCALL);
    AstNode* arr = node->getMember(0);
    TRI_ASSERT(arr->type == AstNodeType::NODE_TYPE_ARRAY);
    collection = arr->getMember(0)->getString();
    attribute = arr->getMember(1)->getString();
    if(arr->numMembers() > 3){
      limit = arr->getMember(3);
    }
  }
};

AstNode* getAstNode(CalculationNode* c){
  return c->expression()->nodeForModification();
}

Function* getFunction(AstNode const* ast){
  if (ast->type == AstNodeType::NODE_TYPE_FCALL){
    return static_cast<Function*>(ast->getData());
  }
  return nullptr;
}

AstNode* createSubqueryWithLimit(
  ExecutionPlan* plan,
  ExecutionNode* node,
  ExecutionNode* first,
  ExecutionNode* last,
  Variable* lastOutVariable,
  AstNode* limit
  ){
  // Creates subquery of the following form:
  //
  //    singleton
  //        |
  //      first
  //        |
  //       ...
  //        |
  //       last
  //        |
  //     [limit]
  //        |
  //      return
  //
  // The Query is then injected into the plan before the given `node`
  //
  auto* ast = plan->getAst();

  /// singleton
  ExecutionNode* eSingleton = plan->registerNode(
      new SingletonNode(plan, plan->nextId())
  );

  /// return
  ExecutionNode* eReturn = plan->registerNode(
      // link output of index with the return node
      new ReturnNode(plan, plan->nextId(), lastOutVariable)
  );

  /// link nodes together
  first->addDependency(eSingleton);
  eReturn->addDependency(last);

  /// add optional limit node
  if(limit) {
    ExecutionNode* eLimit = plan->registerNode(
      new LimitNode(plan, plan->nextId(), 0 /*offset*/, limit->getIntValue())
    );
    plan->insertAfter(last, eLimit); // inject into plan
  }

  /// create subquery
  Variable* subqueryOutVariable = ast->variables()->createTemporaryVariable();
  ExecutionNode* eSubquery = plan->registerSubquery(
      new SubqueryNode(plan, plan->nextId(), eReturn, subqueryOutVariable)
  );

  plan->insertBefore(node, eSubquery);

  // this replaces the FunctionCall-AstNode in the
  // expression of the calculation node.
  return ast->createNodeReference(subqueryOutVariable);

}

AstNode* replaceNearOrWithin(AstNode* funAstNode, ExecutionNode* calcNode, ExecutionPlan* plan, bool isNear){
  auto* ast = plan->getAst();
  auto* query = ast->query();
  auto* trx = query->trx();
  nearOrWithinParams params(funAstNode,isNear);
  bool supportLegacy = true;
  (void) supportLegacy;

  // RETURN (
  //  FOR d IN col
  //    SORT DISTANCE(d.lat, d.long, param.lat, param.lon)
  //    MERGE(d, { param.distname : DISTANCE(d.lat, d.long, param.lat, param.lon)})
  //    LIMIT param.limit
  //
  //   RETURN d MERGE {param.distname : calculated_distance}
  // )

  /// index
  //  we create this first as creation of this node is more
  //  likely to fail than the creation of other nodes

  //  index - part 1 - figure out index to use

  auto& vocbase = trx->vocbase();
  auto* aqlCollection = query->collections()->get(params.collection);
  Variable* enumerateOutVariable = ast->variables()->createTemporaryVariable();
  ExecutionNode* eEnumerate = plan->registerNode(
      // link output of index with the return node
      new EnumerateCollectionNode(
            plan, plan->nextId(),
            &vocbase, aqlCollection,
            enumerateOutVariable, false
      )
  );

  //// build sort codition

  auto* docRef = ast->createNodeReference(enumerateOutVariable);
  AstNode* accessNodeLat = docRef;
  AstNode* accessNodeLon = docRef;
  bool indexFound = false;

	std::vector<basics::AttributeName> field;
	auto indexes = trx->indexesForCollection(params.collection);
	for(auto& idx : indexes){
		if(Index::isGeoIndex(idx->type())) {
      // we take the first index that is found

      bool isGeo1 = idx->type() == Index::IndexType::TRI_IDX_TYPE_GEO1_INDEX && supportLegacy;
      bool isGeo2 = idx->type() == Index::IndexType::TRI_IDX_TYPE_GEO2_INDEX && supportLegacy;
      bool isGeo = idx->type() == Index::IndexType::TRI_IDX_TYPE_GEO_INDEX;

      std::vector<arangodb::basics::AttributeName> accessLatitude;
      std::vector<arangodb::basics::AttributeName> accessLongitude;

      auto fieldNum = idx->fields().size();
      if ((isGeo2 || isGeo) && fieldNum == 2) { // individual fields
        accessLatitude = idx->fields()[0];
        accessLongitude  = idx->fields()[1];

        for(auto const& part : accessLatitude){
          char const* p = query->registerString(part.name.data(), part.name.size());
          accessNodeLat = ast->createNodeAttributeAccess(accessNodeLat, p, part.name.size());
        }

        for(auto const& part : accessLongitude){
          char const* p = query->registerString(part.name.data(), part.name.size());
          accessNodeLon = ast->createNodeAttributeAccess(accessNodeLon, p, part.name.size());
        }
        indexFound = true;
      } else if ((isGeo1 || isGeo) && fieldNum == 1) {
        accessLongitude = idx->fields()[0];

        for(auto const& part : accessLongitude){
          char const* p = query->registerString(part.name.data(), part.name.size());
          accessNodeLon = ast->createNodeAttributeAccess(accessNodeLon, p, part.name.size());
          accessNodeLat = ast->createNodeAttributeAccess(accessNodeLat, p, part.name.size());
        }


        VPackBuilder builder;
        idx->toVelocyPack(builder,true,false);
        bool geoJson = basics::VelocyPackHelper::getBooleanValue(builder.slice(), "geoJson", false);

        accessNodeLat = ast->createNodeIndexedAccess(accessNodeLat, ast->createNodeValueInt(geoJson ? 1 : 0));
        accessNodeLon = ast->createNodeIndexedAccess(accessNodeLon, ast->createNodeValueInt(geoJson ? 0 : 1));
        indexFound = true;
      } // if isGeo 1 or 2
      break;
    }

  } // for index in collection


  if(!indexFound) {
    LOG_DEVEL << "no access path";
    return nullptr;
  }

  auto* argsArray = ast->createNodeArray();
  argsArray->addMember(accessNodeLat);
  argsArray->addMember(accessNodeLon);
  argsArray->addMember(params.latitude);
  argsArray->addMember(params.longitude);

  auto* funDist = ast->createNodeFunctionCall(TRI_CHAR_LENGTH_PAIR("DISTANCE"), argsArray);

  AstNode* expressionAst = funDist;
  if(!isNear){
    expressionAst = ast->createNodeBinaryOperator(
      AstNodeType::NODE_TYPE_OPERATOR_BINARY_LE, funDist ,params.radius
    );
  }

  //Calculation Node will acquire ownership
  Expression* calcExpr = new Expression(plan, ast, expressionAst);

  // put condition into calculation node
  Variable* calcOutVariable = ast->variables()->createTemporaryVariable();
  ExecutionNode* eCalc = plan->registerNode(
      new CalculationNode(plan, plan->nextId(), calcExpr, nullptr, calcOutVariable)
  );
  eCalc->addDependency(eEnumerate);

  ExecutionNode* eSortOrFilter = nullptr;
  if(isNear){
    // use calculation node in sort node
    SortElementVector sortElements { SortElement{ calcOutVariable, /*asc*/ true} }; //CHECKME
    eSortOrFilter = plan->registerNode(
        new SortNode(plan, plan->nextId(), sortElements, false)
    );
  } else {
    eSortOrFilter = plan->registerNode(
        new FilterNode(plan, plan->nextId(), calcOutVariable)
    );
  }
  eSortOrFilter->addDependency(eCalc);

  if(params.distanceName) { //return without merging the distance into the result

    AstNode* elem = nullptr;
    AstNode* funDistMerge = nullptr;
    if(isNear){
      funDistMerge = ast->createNodeReference(calcOutVariable);
    } else {
      //FIXME: recycles Ast directly - must probably be fixed;
      funDistMerge = funDist;
    }
    if(params.distanceName->isConstant()){
      elem = ast->createNodeObjectElement(params.distanceName->getStringValue()
                                         ,params.distanceName->getStringLength()
                                         ,funDistMerge);
    } else {
      elem = ast->createNodeCalculatedObjectElement(params.distanceName, funDistMerge );
    }
    auto* obj = ast->createNodeObject();
    obj->addMember(elem);

    auto* argsArrayMerge = ast->createNodeArray();
    argsArrayMerge->addMember(docRef);
    argsArrayMerge->addMember(obj);

    auto* funMerge = ast->createNodeFunctionCall(TRI_CHAR_LENGTH_PAIR("MERGE"), argsArrayMerge);

    Variable* calcMergeOutVariable = ast->variables()->createTemporaryVariable();
    Expression* calcMergeExpr = new Expression(plan, ast, funMerge);
    ExecutionNode* eCalcMerge = plan->registerNode(
      new CalculationNode(plan, plan->nextId(), calcMergeExpr, nullptr, calcMergeOutVariable)
    );
    plan->insertAfter(eSortOrFilter, eCalcMerge);

    return createSubqueryWithLimit(plan, calcNode, eEnumerate, eCalcMerge, calcMergeOutVariable, params.limit);
  }

  return createSubqueryWithLimit(plan, calcNode,
                                eEnumerate /* first */, eSortOrFilter /* last */,
                                enumerateOutVariable, params.limit);
}

AstNode* replaceFullText(AstNode* funAstNode, ExecutionNode* calcNode, ExecutionPlan* plan){
  auto* ast = plan->getAst();
  auto* query = ast->query();
  auto* trx = query->trx();

  fulltextParams params(funAstNode);

  /// index
  //  we create this first as creation of this node is more
  //  likely to fail than the creation of other nodes

  //  index - part 1 - figure out index to use
  std::shared_ptr<arangodb::Index> index = nullptr;
	std::vector<basics::AttributeName> field;
	TRI_ParseAttributeString(params.attribute, field, false);
	auto indexes = trx->indexesForCollection(params.collection);
	for(auto& idx : indexes){
		if(idx->type() == arangodb::Index::IndexType::TRI_IDX_TYPE_FULLTEXT_INDEX) {
			if(basics::AttributeName::isIdentical(idx->fields()[0], field, false /*ignore expansion in last?!*/)) {
				index = idx;
				break;
			}
		}
	}

	if(!index){ // not found or error
    LOG_DEVEL << "no valid index found";
		return nullptr;
	}

  // index part 2 - get remaining vars required for index creation
  auto& vocbase = trx->vocbase();
  auto* aqlCollection = query->collections()->get(params.collection);
	auto condition = std::make_unique<Condition>(ast);
	condition->andCombine(funAstNode);
	// create a fresh out variable
  Variable* indexOutVariable = ast->variables()->createTemporaryVariable();

  ExecutionNode* eIndex = plan->registerNode(
    new IndexNode(
      plan,
      plan->nextId(),
      &vocbase,
      aqlCollection,
      indexOutVariable,
      std::vector<transaction::Methods::IndexHandle> {
				transaction::Methods::IndexHandle{index}
      },
      std::move(condition),
      IndexIteratorOptions()
    )
  );

  return createSubqueryWithLimit(plan, calcNode, eIndex, eIndex, indexOutVariable, params.limit);
}

} // namespace

void arangodb::aql::replaceJSFunctions(Optimizer* opt
                                      ,std::unique_ptr<ExecutionPlan> plan
                                      ,OptimizerRule const* rule){

  bool modified = false;

  SmallVector<ExecutionNode*>::allocator_type::arena_type a;
  SmallVector<ExecutionNode*> nodes{a};
  plan->findNodesOfType(nodes, ExecutionNode::CALCULATION, true);

  for(auto const& node : nodes){
    auto visitor = [&modified, &node, &plan](AstNode* astnode){
      auto* fun = getFunction(astnode);
      AstNode* replacement = nullptr;
      if(fun){
        if (fun->name == std::string("NEAR")){
          replacement = replaceNearOrWithin(astnode, node, plan.get(), true /*isNear*/);
        }
        if (fun->name == std::string("WITHIN")){
          replacement = replaceNearOrWithin(astnode, node, plan.get(), false /*isNear*/);
        }
        if (fun->name == std::string("FULLTEXT")){
          replacement = replaceFullText(astnode, node,plan.get());
        }
      }
      if (replacement) {
        modified = true;
        return replacement;
      }
      return astnode;
    };

    // replace root node if it was modified
    // TraverseAndModify has no access to roots parent
    CalculationNode* calc = static_cast<CalculationNode*>(node);
    auto* original = getAstNode(calc);
    auto* replacement = Ast::traverseAndModify(original,visitor);

    if (replacement != original) {
      calc->expression()->replaceNode(replacement);
    }
  }

  opt->addPlan(std::move(plan), rule, modified);

}; // replaceJSFunctions
