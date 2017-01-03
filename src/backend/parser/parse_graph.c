/*
 * parse_graph.c
 *	  handle clauses for graph in parser
 *
 * Copyright (c) 2016 by Bitnine Global, Inc.
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_graph.c
 */

#include "postgres.h"

#include "ag_const.h"
#include "access/sysattr.h"
#include "catalog/ag_graph_fn.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "nodes/graphnodes.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_cte.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_graph.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parser.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#define CYPHER_SUBQUERY_ALIAS	"_"
#define CYPHER_OPTMATCH_ALIAS	"_o"
#define CYPHER_VLR_WITH_ALIAS	"_vlr"
#define CYPHER_VLR_EDGE_ALIAS	"_e"

#define VLR_COLNAME_START		"start"
#define VLR_COLNAME_END			"end"
#define VLR_COLNAME_LEVEL		"level"
#define VLR_COLNAME_PATH		"path"

#define EDGE_UNION_START_ID		"_start"
#define EDGE_UNION_END_ID		"_end"

typedef struct
{
	char	   *varname;		/* variable assigned to the node */
	char	   *labname;		/* final label of the vertex */
	bool		prop_constr;	/* has property constraints? */
} NodeInfo;

typedef struct
{
	Index		varno;			/* of the RTE */
	AttrNumber	varattno;		/* in the target list */
	Node	   *prop_constr;	/* property constraints of the element */
} ElemQual;

typedef struct
{
	Index		varno;			/* of the RTE */
	AttrNumber	varattno;		/* in the target list */
	char	   *labname;		/* label of the vertex */
	bool		nullable;		/* is this nullable? */
	Expr	   *expr;			/* resolved vertex */
} FutureVertex;

#define FVR_DONT_RESOLVE		0x01
#define FVR_IGNORE_NULLABLE		0x02
#define FVR_PRESERVE_VAR_REF	0x04

typedef struct
{
	ParseState *pstate;
	int			flags;
	int			sublevels_up;
} resolve_future_vertex_context;

/* projection (RETURN and WITH) */
static void checkNameInItems(ParseState *pstate, List *items, List *targetList);

/* MATCH - OPTIONAL */
static RangeTblEntry *transformMatchOptional(ParseState *pstate,
											 CypherClause *clause);
/* MATCH - preprocessing */
static bool hasPropConstr(List *pattern);
static void collectNodeInfo(ParseState *pstate, List *pattern);
static void addNodeInfo(ParseState *pstate, CypherNode *cnode);
static NodeInfo *getNodeInfo(ParseState *pstate, char *varname);
static NodeInfo *findNodeInfo(ParseState *pstate, char *varname);
static List *makeComponents(List *pattern);
static bool isPathConnectedTo(CypherPath *path, List *component);
static bool arePathsConnected(CypherPath *path1, CypherPath *path2);
/* MATCH - transform */
static Node *transformComponents(ParseState *pstate, List *components,
								 List **targetList);
static Node *transformMatchNode(ParseState *pstate, CypherNode *cnode,
								bool force, List **targetList);
static RangeTblEntry *transformMatchRel(ParseState *pstate, CypherRel *crel,
										List **targetList);
static RangeTblEntry *transformMatchSR(ParseState *pstate, CypherRel *crel,
									   List **targetList);
static RangeTblEntry *addEdgeUnion(ParseState *pstate, char *edge_label,
								   int location, Alias *alias);
static Node *genEdgeUnion(char *edge_label, int location);
static void setInitialVidForVLR(ParseState *pstate, CypherRel *crel,
								Node *vertex, CypherRel *prev_crel,
								RangeTblEntry *prev_edge);
static RangeTblEntry *transformMatchVLR(ParseState *pstate, CypherRel *crel,
										List **targetList);
static SelectStmt *genSelectLeftVLR(ParseState *pstate, CypherRel *crel);
static SelectStmt *genSelectRightVLR(ParseState *pstate, CypherRel *crel);
static RangeSubselect *genEdgeUnionVLR(char *edge_label);
static SelectStmt *genSelectWithVLR(ParseState *pstate,
									CypherRel *crel, WithClause *with);
static RangeTblEntry *transformVLRtoRTE(ParseState *pstate, SelectStmt *vlr,
										Alias *alias);
static bool isZeroLengthVLR(CypherRel *crel);
static void getCypherRelType(CypherRel *crel, char **typname, int *typloc);
static Node *addQualRelPath(ParseState *pstate, Node *qual,
							CypherRel *prev_crel, RangeTblEntry *prev_edge,
							CypherRel *crel, RangeTblEntry *edge);
static Node *addQualNodeIn(ParseState *pstate, Node *qual, Node *vertex,
						   CypherRel *crel, RangeTblEntry *edge, bool prev);
static char *getEdgeColname(CypherRel *crel, bool prev);
static bool isFutureVertexExpr(Node *vertex);
static void setFutureVertexExprId(ParseState *pstate, Node *vertex,
								 CypherRel *crel, RangeTblEntry *edge,
								 bool prev);
static Node *addQualUniqueEdges(ParseState *pstate, Node *qual, List *ueids,
								List *ueidarrs);
/* MATCH - quals */
static void addElemQual(ParseState *pstate, AttrNumber varattno,
						Node *prop_constr);
static void adjustElemQuals(List *elem_quals, RangeTblEntry *rte, int rtindex);
static Node *transformElemQuals(ParseState *pstate, Node *qual);
/* MATCH - future vertex */
static void addFutureVertex(ParseState *pstate, AttrNumber varattno,
							char *labname);
static FutureVertex *findFutureVertex(ParseState *pstate, Index varno,
									  AttrNumber varattno, int sublevels_up);
static void adjustFutureVertices(List *future_vertices, RangeTblEntry *rte,
								 int rtindex);
static Node *resolve_future_vertex(ParseState *pstate, Node *node, int flags);
static Node *resolve_future_vertex_mutator(Node *node,
										   resolve_future_vertex_context *ctx);
static void resolveFutureVertex(ParseState *pstate, FutureVertex *fv,
								bool ignore_nullable);
static RangeTblEntry *makeVertexRTE(ParseState *parentParseState, char *varname,
									char *labname);
static List *removeResolvedFutureVertices(List *future_vertices);

/* CREATE */
static List *transformCreatePattern(ParseState *pstate, List *pattern,
									List **targetList);
static GraphVertex *transformCreateNode(ParseState *pstate, CypherNode *cnode,
										List **targetList);
static GraphEdge *transformCreateRel(ParseState *pstate, CypherRel *crel,
									 List **targetList);

/* SET/REMOVE */
static List *transformSetPropList(ParseState *pstate, RangeTblEntry *rte,
								  List *items);
static GraphSetProp *transformSetProp(ParseState *pstate, RangeTblEntry *rte,
									  CypherSetProp *sp);

/* common */
static bool isNodeForRef(CypherNode *cnode);
static Node *transformPropMap(ParseState *pstate, Node *expr,
							  ParseExprKind exprKind);
static Node *preprocessPropMap(Node *expr);

/* transform */
static RangeTblEntry *transformClause(ParseState *pstate, Node *clause);
static RangeTblEntry *transformClauseImpl(ParseState *pstate, Node *clause,
										  Alias *alias);
static RangeTblEntry *incrementalJoinRTEs(ParseState *pstate, JoinType jointype,
										  RangeTblEntry *l_rte,
										  RangeTblEntry *r_rte,
										  Node *qual, Alias *alias);
static void makeJoinResCols(ParseState *pstate,
							RangeTblEntry *l_rte, RangeTblEntry *r_rte,
							List **res_colnames, List **res_colvars);
static void addRTEtoJoinlist(ParseState *pstate, RangeTblEntry *rte,
							 bool visible);
static void makeExtraFromRTE(ParseState *pstate, RangeTblEntry *rte,
							 RangeTblRef **rtr, ParseNamespaceItem **nsitem,
							 bool visible);
static RangeTblEntry *findRTEfromNamespace(ParseState *pstate, char *refname);
static ParseNamespaceItem *findNamespaceItemForRTE(ParseState *pstate,
												   RangeTblEntry *rte);
static List *makeTargetListFromRTE(ParseState *pstate, RangeTblEntry *rte);
static List *makeTargetListFromJoin(ParseState *pstate, RangeTblEntry *rte);
static TargetEntry *makeWholeRowTarget(ParseState *pstate, RangeTblEntry *rte);
static TargetEntry *findTarget(List *targetList, char *resname);

/* expression - type */
static Node *makeVertexExpr(ParseState *pstate, RangeTblEntry *rte,
							int location);
static Node *makeEdgeExpr(ParseState *pstate, RangeTblEntry *rte, int location);
static Node *makePathVertexExpr(ParseState *pstate, Node *obj);
static Node *makeGraphpath(List *vertices, List *edges, int location);
/* expression - common */
static Node *getColumnVar(ParseState *pstate, RangeTblEntry *rte,
						  char *colname);
static Node *getExprField(Expr *expr, char *fname);
static Alias *makeAliasNoDup(char *aliasname, List *colnames);
static Alias *makeAliasOptUnique(char *aliasname);
static Node *makeArrayExpr(Oid typarray, Oid typoid, List *elems);
static Node *makeTypedRowExpr(List *args, Oid typoid, int location);
static Node *qualAndExpr(Node *qual, Node *expr);

/* parse node */
static ResTarget *makeSimpleResTarget(char *field, char *name);
static ResTarget *makeFieldsResTarget(List *fields, char *name);
static ResTarget *makeResTarget(Node *val, char *name);
static A_Const *makeIntConst(int val);

/* utils */
static char *genUniqueName(void);

Query *
transformCypherSubPattern(ParseState *pstate, CypherSubPattern *subpat)
{
	CypherMatchClause *match;
	CypherClause *clause;
	Query *qry;
	RangeTblEntry *rte;

	match = makeNode(CypherMatchClause);
	match->pattern = subpat->pattern;
	match->where = NULL;
	match->optional = false;

	clause = makeNode(CypherClause);
	clause->detail = (Node *) match;
	clause->prev = NULL;

	qry = makeNode(Query);
	qry->commandType = CMD_SELECT;

	rte = transformClause(pstate, (Node *) clause);

	qry->targetList = makeTargetListFromRTE(pstate, rte);
	if (subpat->kind == CSP_SIZE)
	{
		FuncCall *count;
		TargetEntry *te;

		count = makeFuncCall(list_make1(makeString("count")), NIL, -1);
		count->agg_star = true;

		pstate->p_next_resno = 1;
		te = transformTargetEntry(pstate, (Node *) count, NULL,
								  EXPR_KIND_SELECT_TARGET, NULL, false);

		qry->targetList = list_make1(te);
	}
	markTargetListOrigins(pstate, qry->targetList);

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasAggs = pstate->p_hasAggs;
	if (qry->hasAggs)
		parseCheckAggregates(pstate, qry);

	assign_query_collations(pstate, qry);

	return qry;
}

Query *
transformCypherProjection(ParseState *pstate, CypherClause *clause)
{
	CypherProjection *detail = (CypherProjection *) clause->detail;
	Query	   *qry;
	RangeTblEntry *rte;
	Node	   *qual = NULL;
	int			flags;

	qry = makeNode(Query);
	qry->commandType = CMD_SELECT;

	if (detail->where != NULL)
	{
		Node *where = detail->where;

		AssertArg(detail->kind == CP_WITH);

		detail->where = NULL;
		rte = transformClause(pstate, (Node *) clause);
		detail->where = where;

		qry->targetList = makeTargetListFromRTE(pstate, rte);

		qual = transformWhereClause(pstate, where, EXPR_KIND_WHERE, "WHERE");
		qual = resolve_future_vertex(pstate, qual, 0);
	}
	else if (detail->distinct != NULL || detail->order != NULL ||
			 detail->skip != NULL || detail->limit != NULL)
	{
		List *distinct = detail->distinct;
		List *order = detail->order;
		Node *skip = detail->skip;
		Node *limit = detail->limit;

		/*
		 * detach options so that this function passes through this if statement
		 * when the function is called again recursively
		 */
		detail->distinct = NIL;
		detail->order = NIL;
		detail->skip = NULL;
		detail->limit = NULL;
		rte = transformClause(pstate, (Node *) clause);
		detail->distinct = distinct;
		detail->order = order;
		detail->skip = skip;
		detail->limit = limit;

		qry->targetList = makeTargetListFromRTE(pstate, rte);

		qry->sortClause = transformSortClause(pstate, order, &qry->targetList,
											  EXPR_KIND_ORDER_BY, true, false);

		if (distinct == NIL)
		{
			/* intentionally blank, do nothing */
		}
		else if (linitial(distinct) == NULL)
		{
			qry->distinctClause = transformDistinctClause(pstate,
														  &qry->targetList,
														  qry->sortClause,
														  false);
		}
		else
		{
			qry->distinctClause = transformDistinctOnClause(pstate, distinct,
															&qry->targetList,
															qry->sortClause);
			qry->hasDistinctOn = true;
		}

		qry->limitOffset = transformLimitClause(pstate, skip, EXPR_KIND_OFFSET,
												"OFFSET");
		qry->limitOffset = resolve_future_vertex(pstate, qry->limitOffset, 0);

		qry->limitCount = transformLimitClause(pstate, limit, EXPR_KIND_LIMIT,
											   "LIMIT");
		qry->limitCount = resolve_future_vertex(pstate, qry->limitCount, 0);
	}
	else
	{
		if (clause->prev != NULL)
			transformClause(pstate, clause->prev);

		qry->targetList = transformTargetList(pstate, detail->items,
											  EXPR_KIND_SELECT_TARGET);

		if (detail->kind == CP_WITH)
			checkNameInItems(pstate, detail->items, qry->targetList);

		qry->groupClause = generateGroupClause(pstate, &qry->targetList,
											   qry->sortClause);
	}

	if (detail->kind == CP_WITH)
	{
		ListCell *lt;

		/* try to resolve all target entries except vertex Var */
		foreach(lt, qry->targetList)
		{
			TargetEntry *te = lfirst(lt);

			if (IsA(te->expr, Var) && exprType((Node *) te->expr) == VERTEXOID)
				continue;

			te->expr = (Expr *) resolve_future_vertex(pstate,
													  (Node *) te->expr, 0);
		}

		flags = FVR_DONT_RESOLVE;
	}
	else
	{
		flags = 0;
	}
	qry->targetList = (List *) resolve_future_vertex(pstate,
													 (Node *) qry->targetList,
													 flags);
	markTargetListOrigins(pstate, qry->targetList);

	qual = qualAndExpr(qual, pstate->p_resolved_qual);

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasAggs = pstate->p_hasAggs;
	if (qry->hasAggs)
		parseCheckAggregates(pstate, qry);

	assign_query_collations(pstate, qry);

	return qry;
}

Query *
transformCypherMatchClause(ParseState *pstate, CypherClause *clause)
{
	CypherMatchClause *detail = (CypherMatchClause *) clause->detail;
	Query	   *qry;
	RangeTblEntry *rte;
	Node	   *qual = NULL;

	qry = makeNode(Query);
	qry->commandType = CMD_SELECT;

	/*
	 * since WHERE clause is part of MATCH,
	 * transform OPTIONAL MATCH with its WHERE clause
	 */
	if (detail->optional && clause->prev != NULL)
	{
		/*
		 * NOTE: Should we return a single row with NULL values
		 *       if OPTIONAL MATCH is the first clause and
		 *       there is no result that matches the pattern?
		 */

		rte = transformMatchOptional(pstate, clause);

		qry->targetList = makeTargetListFromJoin(pstate, rte);
	}
	else
	{

		if (!pstate->p_is_match_quals &&
			(detail->where != NULL || hasPropConstr(detail->pattern)))
		{
			int flags = (pstate->p_is_optional_match ? FVR_IGNORE_NULLABLE: 0);

			pstate->p_is_match_quals = true;
			rte = transformClause(pstate, (Node *) clause);

			qry->targetList = makeTargetListFromRTE(pstate, rte);

			qual = transformWhereClause(pstate, detail->where, EXPR_KIND_WHERE,
										"WHERE");
			qual = transformElemQuals(pstate, qual);
			qual = resolve_future_vertex(pstate, qual, flags);
		}
		else
		{
			List *components;

			pstate->p_is_match_quals = false;

			/*
			 * To do this at here is safe since it just uses transformed
			 * expression and does not look over the ancestors of `pstate`.
			 */
			if (clause->prev != NULL)
			{
				rte = transformClause(pstate, clause->prev);

				qry->targetList = makeTargetListFromRTE(pstate, rte);
			}

			collectNodeInfo(pstate, detail->pattern);
			components = makeComponents(detail->pattern);

			qual = transformComponents(pstate, components, &qry->targetList);
			/* there is no need to resolve `qual` here */
		}

		qry->targetList = (List *) resolve_future_vertex(pstate,
													(Node *) qry->targetList,
													FVR_DONT_RESOLVE);
	}
	markTargetListOrigins(pstate, qry->targetList);

	qual = qualAndExpr(qual, pstate->p_resolved_qual);

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

Query *
transformCypherCreateClause(ParseState *pstate, CypherClause *clause)
{
	CypherCreateClause *detail;
	CypherClause *prevclause;
	List	   *pattern;
	Query	   *qry;

	detail = (CypherCreateClause *) clause->detail;
	pattern = detail->pattern;
	prevclause = (CypherClause *) clause->prev;

	/* merge previous CREATE clauses into current CREATE clause */
	while (prevclause != NULL &&
		   cypherClauseTag(prevclause) == T_CypherCreateClause)
	{
		List *prevpattern;

		detail = (CypherCreateClause *) prevclause->detail;

		prevpattern = list_copy(detail->pattern);
		pattern = list_concat(prevpattern, pattern);

		prevclause = (CypherClause *) prevclause->prev;
	}

	qry = makeNode(Query);
	qry->commandType = CMD_GRAPHWRITE;
	qry->graph.writeOp = GWROP_CREATE;
	qry->graph.last = (pstate->parentParseState == NULL);

	if (prevclause != NULL)
	{
		RangeTblEntry *rte;

		rte = transformClause(pstate, (Node *) prevclause);

		qry->targetList = makeTargetListFromRTE(pstate, rte);
	}

	qry->graph.pattern = transformCreatePattern(pstate, pattern,
												&qry->targetList);

	qry->targetList = (List *) resolve_future_vertex(pstate,
													 (Node *) qry->targetList,
													 FVR_DONT_RESOLVE);
	markTargetListOrigins(pstate, qry->targetList);

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, pstate->p_resolved_qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

Query *
transformCypherDeleteClause(ParseState *pstate, CypherClause *clause)
{
	CypherDeleteClause *detail = (CypherDeleteClause *) clause->detail;
	Query	   *qry;
	RangeTblEntry *rte;
	List	   *exprs;
	ListCell   *le;

	/* DELETE cannot be the first clause */
	AssertArg(clause->prev != NULL);

	qry = makeNode(Query);
	qry->commandType = CMD_GRAPHWRITE;
	qry->graph.writeOp = GWROP_DELETE;
	qry->graph.last = (pstate->parentParseState == NULL);
	qry->graph.detach = detail->detach;

	/*
	 * Instead of `resultRelation`, use FROM list because there might be
	 * multiple labels to access.
	 */
	rte = transformClause(pstate, clause->prev);

	/* select all from previous clause */
	qry->targetList = makeTargetListFromRTE(pstate, rte);

	exprs = transformExpressionList(pstate, detail->exprs, EXPR_KIND_OTHER);

	foreach(le, exprs)
	{
		Node	   *expr = lfirst(le);
		Oid			vartype;

		vartype = exprType(expr);
		if (vartype != VERTEXOID && vartype != EDGEOID &&
			vartype != GRAPHPATHOID)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("node, relationship, or path is expected"),
					 parser_errposition(pstate, exprLocation(expr))));

		/*
		 * TODO: `expr` must contain one of the target variables
		 *		 and it mustn't contain aggregate and SubLink's.
		 */
	}
	qry->graph.exprs = exprs;

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

Query *
transformCypherSetClause(ParseState *pstate, CypherClause *clause)
{
	CypherSetClause *detail = (CypherSetClause *) clause->detail;
	Query	   *qry;
	RangeTblEntry *rte;

	/* SET/REMOVE cannot be the first clause */
	AssertArg(clause->prev != NULL);

	qry = makeNode(Query);
	qry->commandType = CMD_GRAPHWRITE;
	qry->graph.writeOp = GWROP_SET;
	qry->graph.last = (pstate->parentParseState == NULL);

	rte = transformClause(pstate, clause->prev);

	qry->targetList = makeTargetListFromRTE(pstate, rte);

	qry->graph.sets = transformSetPropList(pstate, rte, detail->items);

	qry->targetList = (List *) resolve_future_vertex(pstate,
													 (Node *) qry->targetList,
													 FVR_DONT_RESOLVE);

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, pstate->p_resolved_qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

Query *
transformCypherLoadClause(ParseState *pstate, CypherClause *clause)
{
	CypherLoadClause *detail = (CypherLoadClause *) clause->detail;
	RangeVar   *rv = detail->relation;
	Query	   *qry;
	RangeTblEntry *rte;
	TargetEntry *te;

	qry = makeNode(Query);
	qry->commandType = CMD_SELECT;

	if (clause->prev != NULL)
	{
		rte = transformClause(pstate, clause->prev);

		qry->targetList = makeTargetListFromRTE(pstate, rte);
	}

	if (findTarget(qry->targetList, rv->alias->aliasname) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_ALIAS),
				 errmsg("duplicate variable \"%s\"", rv->alias->aliasname)));

	rte = addRangeTableEntry(pstate, rv, rv->alias,
							 interpretInhOption(rv->inhOpt), true);
	addRTEtoJoinlist(pstate, rte, false);

	te = makeWholeRowTarget(pstate, rte);
	qry->targetList = lappend(qry->targetList, te);

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	assign_query_collations(pstate, qry);

	return qry;
}

/* check whether resulting columns have a name or not */
static void
checkNameInItems(ParseState *pstate, List *items, List *targetList)
{
	ListCell   *li;
	ListCell   *lt;

	forboth(li, items, lt, targetList)
	{
		ResTarget *res = lfirst(li);
		TargetEntry *te = lfirst(lt);

		if (res->name != NULL)
			continue;

		if (!IsA(te->expr, Var))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("expression in WITH must be aliased (use AS)"),
					 parser_errposition(pstate, exprLocation(res->val))));
	}
}

/* See transformFromClauseItem() */
static RangeTblEntry *
transformMatchOptional(ParseState *pstate, CypherClause *clause)
{
	CypherMatchClause *detail = (CypherMatchClause *) clause->detail;
	RangeTblEntry *l_rte;
	Alias	   *r_alias;
	RangeTblEntry *r_rte;
	Node	   *prevclause;
	Node	   *qual;
	Alias	   *alias;

	/* transform LEFT */
	l_rte = transformClause(pstate, clause->prev);

	/*
	 * Transform RIGHT. Prevent `clause` from being transformed infinitely.
	 * `p_cols_visible` of `l_rte` must be set to allow `r_rte` to see columns
	 * of `l_rte` by their name.
	 */

	prevclause = clause->prev;
	clause->prev = NULL;
	detail->optional = false;

	pstate->p_lateral_active = true;
	pstate->p_is_optional_match = true;

	r_alias = makeAliasNoDup(CYPHER_OPTMATCH_ALIAS, NIL);
	r_rte = transformClauseImpl(pstate, (Node *) clause, r_alias);

	pstate->p_is_optional_match = false;
	pstate->p_lateral_active = false;

	detail->optional = true;
	clause->prev = prevclause;

	qual = makeBoolConst(true, false);
	alias = makeAliasNoDup(CYPHER_SUBQUERY_ALIAS, NIL);

	return incrementalJoinRTEs(pstate, JOIN_LEFT, l_rte, r_rte, qual, alias);
}

static bool
hasPropConstr(List *pattern)
{
	ListCell *lp;

	foreach(lp, pattern)
	{
		CypherPath *p = lfirst(lp);
		ListCell *le;

		foreach(le, p->chain)
		{
			Node *elem = lfirst(le);

			if (IsA(elem, CypherNode))
			{
				CypherNode *cnode = (CypherNode *) elem;

				if (cnode->prop_map != NULL)
					return true;
			}
			else
			{
				CypherRel *crel = (CypherRel *) elem;

				Assert(IsA(elem, CypherRel));

				if (crel->prop_map != NULL)
					return true;
			}
		}
	}

	return false;
}

static void
collectNodeInfo(ParseState *pstate, List *pattern)
{
	ListCell *lp;

	foreach(lp, pattern)
	{
		CypherPath *p = lfirst(lp);
		ListCell *le;

		foreach(le, p->chain)
		{
			CypherNode *cnode = lfirst(le);

			if (IsA(cnode, CypherNode))
				addNodeInfo(pstate, cnode);
		}
	}
}

static void
addNodeInfo(ParseState *pstate, CypherNode *cnode)
{
	char	   *varname = getCypherName(cnode->variable);
	char	   *labname = getCypherName(cnode->label);
	NodeInfo   *ni;

	if (varname == NULL)
		return;

	ni = findNodeInfo(pstate, varname);
	if (ni == NULL)
	{
		ni = palloc(sizeof(*ni));
		ni->varname = varname;
		ni->labname = labname;
		ni->prop_constr = (cnode->prop_map != NULL);

		pstate->p_node_info_list = lappend(pstate->p_node_info_list, ni);
		return;
	}

	if (ni->labname == NULL)
	{
		ni->labname = labname;
	}
	else
	{
		if (labname != NULL && strcmp(ni->labname, labname) != 0)
		{
			int varloc = getCypherNameLoc(cnode->variable);

			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("label conflict on node \"%s\"", varname),
					 parser_errposition(pstate, varloc)));
		}
	}
	ni->prop_constr = (ni->prop_constr || (cnode->prop_map != NULL));
}

static NodeInfo *
getNodeInfo(ParseState *pstate, char *varname)
{
	NodeInfo *ni;

	if (varname == NULL)
		return NULL;

	ni = findNodeInfo(pstate, varname);

	return ni;
}

static NodeInfo *
findNodeInfo(ParseState *pstate, char *varname)
{
	ListCell *le;

	foreach(le, pstate->p_node_info_list)
	{
		NodeInfo *ni = lfirst(le);

		if (strcmp(ni->varname, varname) == 0)
			return ni;
	}

	return NULL;
}

/* make connected components */
static List *
makeComponents(List *pattern)
{
	List	   *components = NIL;
	ListCell   *lp;

	foreach(lp, pattern)
	{
		CypherPath *p = lfirst(lp);
		List	   *repr;
		ListCell   *lc;
		List	   *c;
		ListCell   *prev;

		/* find the first connected component */
		repr = NIL;
		foreach(lc, components)
		{
			c = lfirst(lc);

			if (isPathConnectedTo(p, c))
			{
				repr = c;
				break;
			}
		}

		/*
		 * if there is no matched connected component,
		 * make a new connected component which is a list of CypherPath's
		 */
		if (repr == NIL)
		{
			c = list_make1(p);
			components = lappend(components, c);
			continue;
		}

		/* find other connected components and merge them to `repr` */
		Assert(lc != NULL);
		prev = lc;
		for_each_cell(lc, lnext(lc))
		{
			c = lfirst(lc);

			if (isPathConnectedTo(p, c))
			{
				list_concat(repr, c);

				components = list_delete_cell(components, lc, prev);
				lc = prev;
			}
			else
			{
				prev = lc;
			}
		}

		/* add the path to `repr` */
		repr = lappend(repr, p);
	}

	Assert(components != NIL);
	return components;
}

static bool
isPathConnectedTo(CypherPath *path, List *component)
{
	ListCell *lp;

	foreach(lp, component)
	{
		CypherPath *p = lfirst(lp);

		if (arePathsConnected(p, path))
			return true;
	}

	return false;
}

static bool
arePathsConnected(CypherPath *path1, CypherPath *path2)
{
	ListCell *le1;

	foreach(le1, path1->chain)
	{
		CypherNode *cnode1 = lfirst(le1);
		char	   *varname1;
		ListCell   *le2;

		/* node variables are the only concern */
		if (!IsA(cnode1, CypherNode))
			continue;

		varname1 = getCypherName(cnode1->variable);
		/* treat it as a unique node */
		if (varname1 == NULL)
			continue;

		foreach(le2, path2->chain)
		{
			CypherNode *cnode2 = lfirst(le2);
			char	   *varname2;

			if (!IsA(cnode2, CypherNode))
				continue;

			varname2 = getCypherName(cnode2->variable);
			if (varname2 == NULL)
				continue;

			if (strcmp(varname1, varname2) == 0)
				return true;
		}
	}

	return false;
}

static Node *
transformComponents(ParseState *pstate, List *components, List **targetList)
{
	Node	   *qual = NULL;
	ListCell   *lc;

	foreach(lc, components)
	{
		List	   *c = lfirst(lc);
		ListCell   *lp;
		List	   *ueids = NIL;

		foreach(lp, c)
		{
			CypherPath *p = lfirst(lp);
			char	   *pathname = getCypherName(p->variable);
			int			pathloc = getCypherNameLoc(p->variable);
			bool		out = (pathname != NULL);
			TargetEntry *te;
			ListCell   *le;
			CypherNode *cnode;
			Node	   *vertex;
			CypherRel  *prev_crel = NULL;
			RangeTblEntry *prev_edge = NULL;
			List	   *pvs = NIL;
			List	   *pes = NIL;

			te = findTarget(*targetList, pathname);
			if (te != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_ALIAS),
						 errmsg("duplicate variable \"%s\"", pathname),
						 parser_errposition(pstate, pathloc)));

			if (te == NULL && pathname != NULL)
			{
				if (colNameToVar(pstate, pathname, false, pathloc) != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_ALIAS),
							 errmsg("duplicate variable \"%s\"", pathname),
							 parser_errposition(pstate, pathloc)));
			}

			pstate->p_last_edge = NULL;
			le = list_head(p->chain);
			for (;;)
			{
				CypherRel *crel;
				RangeTblEntry *edge;

				cnode = lfirst(le);

				/* `cnode` is the first node in the path */
				if (prev_crel == NULL)
				{
					bool zero;

					le = lnext(le);

					/* vertex only path */
					if (le == NULL)
					{
						vertex = transformMatchNode(pstate, cnode, true,
													targetList);
						break;
					}

					crel = lfirst(le);

					/*
					 * if `crel` is zero-length VLR, get RTE of `cnode`
					 * because `crel` needs `id` column of the RTE
					 */
					zero = isZeroLengthVLR(crel);
					vertex = transformMatchNode(pstate, cnode,
												(zero || out), targetList);

					setInitialVidForVLR(pstate, crel, vertex, NULL, NULL);
					edge = transformMatchRel(pstate, crel, targetList);

					qual = addQualNodeIn(pstate, qual, vertex, crel, edge,
										 false);
				}
				else
				{
					vertex = transformMatchNode(pstate, cnode, out, targetList);
					qual = addQualNodeIn(pstate, qual, vertex,
										 prev_crel, prev_edge, true);

					le = lnext(le);
					/* end of the path */
					if (le == NULL)
						break;

					crel = lfirst(le);
					setInitialVidForVLR(pstate, crel, vertex,
										prev_crel, prev_edge);
					edge = transformMatchRel(pstate, crel, targetList);
					qual = addQualRelPath(pstate, qual,
										  prev_crel, prev_edge, crel, edge);
				}

				/* uniqueness */
				if (crel->varlen == NULL)
				{
					Node *eid;

					eid = getColumnVar(pstate, edge, AG_ELEM_LOCAL_ID);
					ueids = list_append_unique(ueids, eid);
				}

				if (out)
				{
					Assert(vertex != NULL);
					pvs = lappend(pvs, makePathVertexExpr(pstate, vertex));
					pes = lappend(pes, makeEdgeExpr(pstate, edge, -1));
				}

				prev_crel = crel;
				prev_edge = edge;
				pstate->p_last_edge = (Node *) edge;

				le = lnext(le);
			}

			if (out)
			{
				Node *graphpath;
				TargetEntry *te;

				Assert(vertex != NULL);
				pvs = lappend(pvs, makePathVertexExpr(pstate, vertex));

				graphpath = makeGraphpath(pvs, pes, pathloc);
				te = makeTargetEntry((Expr *) graphpath,
									 (AttrNumber) pstate->p_next_resno++,
									 pathname,
									 false);

				*targetList = lappend(*targetList, te);
			}
		}

		qual = addQualUniqueEdges(pstate, qual, ueids, NIL);
	}

	return qual;
}

static Node *
transformMatchNode(ParseState *pstate, CypherNode *cnode, bool force,
				   List **targetList)
{
	char	   *varname = getCypherName(cnode->variable);
	int			varloc = getCypherNameLoc(cnode->variable);
	TargetEntry *te;
	NodeInfo   *ni = NULL;
	char	   *labname;
	int			labloc;
	bool		prop_constr;
	Const	   *id;
	Const	   *prop_map;
	Node	   *vertex;

	/*
	 * If a vertex with the same variable is already in the target list,
	 * - the vertex is from the previous clause or
	 * - a node with the same variable in the pattern are already processed,
	 * so skip `cnode`.
	 */
	te = findTarget(*targetList, varname);
	if (te != NULL)
	{
		RangeTblEntry *rte;

		if (exprType((Node *) te->expr) != VERTEXOID)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_ALIAS),
					 errmsg("duplicate variable \"%s\"", varname),
					 parser_errposition(pstate, varloc)));

		addElemQual(pstate, te->resno, cnode->prop_map);

		rte = findRTEfromNamespace(pstate, varname);
		if (rte == NULL)
		{
			/*
			 * `te` can be from the previous clause or the pattern.
			 * If it is from the pattern, it should be an actual vertex or
			 * a future vertex
			 */
			return (Node *) te;
		}
		else
		{
			/* previously returned RTE_RELATION by this function */
			return (Node *) rte;
		}
	}

	/*
	 * try to find the variable when this pattern is within an OPTIONAL MATCH
	 * or a sub-SELECT
	 */
	if (te == NULL && varname != NULL)
	{
		Var *col;

		col = (Var *) colNameToVar(pstate, varname, false, varloc);
		if (col != NULL)
		{
			FutureVertex *fv;

			if (cnode->label != NULL || exprType((Node *) col) != VERTEXOID)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_ALIAS),
						 errmsg("duplicate variable \"%s\"", varname),
						 parser_errposition(pstate, varloc)));

			te = makeTargetEntry((Expr *) col,
								 (AttrNumber) pstate->p_next_resno++,
								 varname,
								 false);

			addElemQual(pstate, te->resno, cnode->prop_map);
			*targetList = lappend(*targetList, te);

			/* `col` can be a future vertex */
			fv = findFutureVertex(pstate, col->varno, col->varattno,
								  col->varlevelsup);
			if (fv != NULL)
				addFutureVertex(pstate, te->resno, fv->labname);

			return (Node *) te;
		}
	}

	if (varname == NULL)
	{
		labname = getCypherName(cnode->label);
		labloc = getCypherNameLoc(cnode->label);
		prop_constr = (cnode->prop_map != NULL);
	}
	else
	{
		ni = getNodeInfo(pstate, varname);
		Assert(ni != NULL);

		labname = ni->labname;
		labloc = -1;
		prop_constr = ni->prop_constr;
	}
	if (labname == NULL)
		labname = AG_VERTEX;

	/*
	 * If `cnode` has a label constraint or a property constraint, return RTE.
	 *
	 * if `cnode` is in a path, return RTE because the path must consit of
	 * valid vertices.
	 * If there is no previous relationship of `cnode` in the path and
	 * the next relationship of `cnode` is zero-length, return RTE
	 * because the relationship needs starting point.
	 */
	if (strcmp(labname, AG_VERTEX) != 0 || prop_constr || force)
	{
		RangeVar   *r;
		Alias	   *alias;
		RangeTblEntry *rte;

		r = makeRangeVar(get_graph_path(), labname, labloc);
		alias = makeAliasOptUnique(varname);

		/* set `ihn` to true because we should scan all derived tables */
		rte = addRangeTableEntry(pstate, r, alias, true, true);
		addRTEtoJoinlist(pstate, rte, false);

		if (varname != NULL || prop_constr)
		{
			te = makeTargetEntry((Expr *) makeVertexExpr(pstate, rte, varloc),
								 (AttrNumber) pstate->p_next_resno++,
								 alias->aliasname,
								 false);

			addElemQual(pstate, te->resno, cnode->prop_map);
			*targetList = lappend(*targetList, te);
		}

		/* return RTE to help the caller can access columns directly */
		return (Node *) rte;
	}

	/* this node is just a placeholder for relationships */
	if (varname == NULL)
		return NULL;

	/*
	 * `cnode` is assigned to the variable `varname` but there is a chance to
	 * omit the RTE for `cnode` if no expression uses properties of `cnode`.
	 * So, return a (invalid) future vertex at here for later use.
	 */

	id = makeNullConst(GRAPHIDOID, -1, InvalidOid);
	prop_map = makeNullConst(JSONBOID, -1, InvalidOid);
	vertex = makeTypedRowExpr(list_make2(id, prop_map), VERTEXOID, varloc);
	te = makeTargetEntry((Expr *) vertex,
						 (AttrNumber) pstate->p_next_resno++,
						 varname,
						 false);

	/* there is no need to addElemQual() here */
	*targetList = lappend(*targetList, te);

	addFutureVertex(pstate, te->resno, labname);

	return (Node *) te;
}

static RangeTblEntry *
transformMatchRel(ParseState *pstate, CypherRel *crel, List **targetList)
{
	char	   *varname = getCypherName(crel->variable);
	int			varloc = getCypherNameLoc(crel->variable);
	TargetEntry *te;

	/* all relationships must be unique */
	te = findTarget(*targetList, varname);
	if (te != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_ALIAS),
				 errmsg("duplicate variable \"%s\"", varname),
				 parser_errposition(pstate, varloc)));

	if (te == NULL && varname != NULL)
	{
		if (colNameToVar(pstate, varname, false, varloc) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_ALIAS),
					 errmsg("duplicate variable \"%s\"", varname),
					 parser_errposition(pstate, varloc)));
	}

	if (crel->varlen == NULL)
		return transformMatchSR(pstate, crel, targetList);
	else
		return transformMatchVLR(pstate, crel, targetList);
}

static RangeTblEntry *
transformMatchSR(ParseState *pstate, CypherRel *crel, List **targetList)
{
	char	   *varname = getCypherName(crel->variable);
	int			varloc = getCypherNameLoc(crel->variable);
	char	   *typname;
	int			typloc;
	Alias	   *alias;
	RangeTblEntry *rte;

	getCypherRelType(crel, &typname, &typloc);

	alias = makeAliasOptUnique(varname);

	if (crel->direction == CYPHER_REL_DIR_NONE)
	{
		rte = addEdgeUnion(pstate, typname, typloc, alias);
	}
	else
	{
		RangeVar *r;

		r = makeRangeVar(get_graph_path(), typname, typloc);

		rte = addRangeTableEntry(pstate, r, alias, true, true);
	}
	addRTEtoJoinlist(pstate, rte, false);

	if (varname != NULL || crel->prop_map != NULL)
	{
		TargetEntry *te;

		te = makeTargetEntry((Expr *) makeEdgeExpr(pstate, rte, varloc),
							 (AttrNumber) pstate->p_next_resno++,
							 alias->aliasname,
							 false);

		addElemQual(pstate, te->resno, crel->prop_map);
		*targetList = lappend(*targetList, te);
	}

	return rte;
}

static RangeTblEntry *
addEdgeUnion(ParseState *pstate, char *edge_label, int location, Alias *alias)
{
	Node	   *u;
	Query	   *qry;
	RangeTblEntry *rte;

	AssertArg(alias != NULL);

	Assert(pstate->p_expr_kind == EXPR_KIND_NONE);
	pstate->p_expr_kind = EXPR_KIND_FROM_SUBSELECT;

	u = genEdgeUnion(edge_label, location);
	qry = parse_sub_analyze(u, pstate, NULL,
							isLockedRefname(pstate, alias->aliasname));

	pstate->p_expr_kind = EXPR_KIND_NONE;

	rte = addRangeTableEntryForSubquery(pstate, qry, alias, false, true);

	return rte;
}

/*
 * SELECT tableoid, ctid, id, start, "end", properties,
 *        start as _start, "end" as _end
 * FROM `get_graph_path()`.`edge_label`
 * UNION
 * SELECT tableoid, ctid, id, start, "end", properties,
 *        "end" as _start, start as _end
 * FROM `get_graph_path()`.`edge_label`
 */
static Node *
genEdgeUnion(char *edge_label, int location)
{
	ResTarget  *tableoid;
	ResTarget  *ctid;
	ResTarget  *id;
	ResTarget  *start;
	ResTarget  *end;
	ResTarget  *prop_map;
	RangeVar   *r;
	SelectStmt *lsel;
	SelectStmt *rsel;
	SelectStmt *u;

	tableoid = makeSimpleResTarget("tableoid", NULL);
	ctid = makeSimpleResTarget("ctid", NULL);
	id = makeSimpleResTarget(AG_ELEM_LOCAL_ID, NULL);
	start = makeSimpleResTarget(AG_START_ID, NULL);
	end = makeSimpleResTarget(AG_END_ID, NULL);
	prop_map = makeSimpleResTarget(AG_ELEM_PROP_MAP, NULL);

	r = makeRangeVar(get_graph_path(), edge_label, location);
	r->inhOpt = INH_YES;

	lsel = makeNode(SelectStmt);
	lsel->targetList = lappend(list_make5(tableoid, ctid, id, start, end),
							   prop_map);
	lsel->fromClause = list_make1(r);

	rsel = copyObject(lsel);

	lsel->targetList = lappend(lsel->targetList,
							   makeSimpleResTarget(AG_START_ID,
												   EDGE_UNION_START_ID));
	lsel->targetList = lappend(lsel->targetList,
							   makeSimpleResTarget(AG_END_ID,
												   EDGE_UNION_END_ID));

	rsel->targetList = lappend(rsel->targetList,
							   makeSimpleResTarget(AG_END_ID,
												   EDGE_UNION_START_ID));
	rsel->targetList = lappend(rsel->targetList,
							   makeSimpleResTarget(AG_START_ID,
												   EDGE_UNION_END_ID));

	u = makeNode(SelectStmt);
	u->op = SETOP_UNION;
	u->all = true;
	u->larg = lsel;
	u->rarg = rsel;

	return (Node *) u;
}

static void
setInitialVidForVLR(ParseState *pstate, CypherRel *crel, Node *vertex,
					CypherRel *prev_crel, RangeTblEntry *prev_edge)
{
	ColumnRef  *cref;

	/* nothing to do */
	if (crel->varlen == NULL)
		return;

	if (vertex == NULL || isFutureVertexExpr(vertex))
	{
		if (prev_crel == NULL)
		{
			pstate->p_vlr_initial_vid = NULL;
			pstate->p_vlr_initial_rte = NULL;
		}
		else
		{
			char *colname;

			colname = getEdgeColname(prev_crel, true);

			cref = makeNode(ColumnRef);
			cref->fields = list_make2(makeString(prev_edge->eref->aliasname),
									  makeString(colname));
			cref->location = -1;

			pstate->p_vlr_initial_vid = (Node *) cref;
			pstate->p_vlr_initial_rte = prev_edge;
		}

		return;
	}

	if (IsA(vertex, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) vertex;

		Assert(rte->rtekind == RTE_RELATION);

		cref = makeNode(ColumnRef);
		cref->fields = list_make2(makeString(rte->eref->aliasname),
								  makeString(AG_ELEM_LOCAL_ID));
		cref->location = -1;

		pstate->p_vlr_initial_vid = (Node *) cref;
		pstate->p_vlr_initial_rte = rte;
	}
	else
	{
		TargetEntry *te = (TargetEntry *) vertex;
		Node *vid;

		AssertArg(IsA(vertex, TargetEntry));

		/* vertex or future vertex */

		cref = makeNode(ColumnRef);
		cref->fields = list_make1(makeString(te->resname));
		cref->location = -1;

		vid = (Node *) makeFuncCall(list_make1(makeString(AG_ELEM_ID)),
									list_make1(cref), -1);

		pstate->p_vlr_initial_vid = vid;
		pstate->p_vlr_initial_rte = NULL;
	}
}

static RangeTblEntry *
transformMatchVLR(ParseState *pstate, CypherRel *crel, List **targetList)
{
	char	   *varname = getCypherName(crel->variable);
	SelectStmt *u;
	CommonTableExpr *cte;
	A_Indices  *indices;
	WithClause *with;
	SelectStmt *vlr;
	Alias	   *alias;
	RangeTblEntry *rte;

	/* UNION ALL */
	u = makeNode(SelectStmt);
	u->op = SETOP_UNION;
	u->all = true;
	u->larg = genSelectLeftVLR(pstate, crel);
	u->rarg = genSelectRightVLR(pstate, crel);

	cte = makeNode(CommonTableExpr);
	cte->ctename = CYPHER_VLR_WITH_ALIAS;
	cte->aliascolnames = list_make2(makeString(VLR_COLNAME_END),
									makeString(VLR_COLNAME_LEVEL));
	if (pstate->p_last_edge != NULL)
	{
		cte->aliascolnames = lappend(cte->aliascolnames,
									 makeString(VLR_COLNAME_START));
	}
	cte->aliascolnames = lappend(cte->aliascolnames,
								 makeString(VLR_COLNAME_PATH));
	cte->ctequery = (Node *) u;
	cte->location = -1;

	indices = (A_Indices *) crel->varlen;
	if (indices->uidx != NULL)
	{
		A_Const	   *lidx;
		A_Const	   *uidx;
		int			base = 0;

		lidx = (A_Const *) indices->lidx;
		if (lidx == NULL || lidx->val.val.ival != 0)
			base = 1;

		uidx = (A_Const *) indices->uidx;

		cte->maxdepth = uidx->val.val.ival - base + 1;
	}

	with = makeNode(WithClause);
	with->ctes = list_make1(cte);
	with->recursive = true;
	with->location = -1;

	vlr = genSelectWithVLR(pstate, crel, with);

	alias = makeAliasOptUnique(varname);
	rte = transformVLRtoRTE(pstate, vlr, alias);

	if (varname != NULL)
	{
		TargetEntry *te;
		Node *var;

		var = getColumnVar(pstate, rte, VLR_COLNAME_PATH);
		te = makeTargetEntry((Expr *) var,
							 (AttrNumber) pstate->p_next_resno++,
							 pstrdup(varname),
							 false);

		*targetList = lappend(*targetList, te);
	}

	return rte;
}

/*
 * -- level == 0
 * VALUES (`id(vertex)`, `id(vertex)`, 0, ARRAY[]::graphid[])
 *
 * -- level > 0, CYPHER_REL_DIR_LEFT
 * SELECT start, "end", 1, ARRAY[id]
 * FROM `get_graph_path()`.`typname`
 * WHERE "end" = `id(vertex)` AND properties @> `crel->prop_map`
 *
 * -- level > 0, CYPHER_REL_DIR_RIGHT
 * SELECT start, "end", 1, ARRAY[id]
 * FROM `get_graph_path()`.`typname`
 * WHERE start = `id(vertex)` AND properties @> `crel->prop_map`
 *
 * -- level > 0, CYPHER_REL_DIR_NONE
 * SELECT start, "end", 1, ARRAY[id]
 * FROM `genEdgeUnionVLR(typname)`
 * WHERE start = `id(vertex)` AND properties @> `crel->prop_map`
 */
static SelectStmt *
genSelectLeftVLR(ParseState *pstate, CypherRel *crel)
{
	Node	   *vid;
	A_ArrayExpr *patharr;
	char	   *typname;
	ResTarget  *end;
	ResTarget  *level;
	Node       *edge;
	List	   *where_args = NIL;
	SelectStmt *sel;
	bool start_out;
	bool path_out = true;

	/*
	 * `vid` is NULL only if
	 * (there is no previous edge of the vertex in the path
	 *  and the vertex is transformed first time in the pattern)
	 * and `crel` is not zero-length
	 */
	vid = pstate->p_vlr_initial_vid;

	start_out = pstate->p_last_edge != NULL;

	if (isZeroLengthVLR(crel))
	{
		List *values;

		values = list_make2(vid, makeIntConst(0));
		if (start_out)
		{
			values = lappend(values, vid);
		}
		if (path_out)
		{
			TypeCast *typecast;
			patharr = makeNode(A_ArrayExpr);
			patharr->location = -1;
			typecast = makeNode(TypeCast);
			typecast->arg = (Node *) patharr;
			typecast->typeName = makeTypeName("_graphid");
			typecast->location = -1;
			values = lappend(values, typecast);
		}
		sel = makeNode(SelectStmt);
		sel->valuesLists = list_make1(values);

		return sel;
	}

	getCypherRelType(crel, &typname, NULL);

	end = makeSimpleResTarget(AG_END_ID, NULL);

	level = makeResTarget((Node *) makeIntConst(1), NULL);

	if (crel->direction == CYPHER_REL_DIR_NONE)
	{
		RangeSubselect *sub;

		sub = genEdgeUnionVLR(typname);
		sub->alias = makeAliasNoDup(CYPHER_VLR_EDGE_ALIAS, NIL);
		edge = (Node *) sub;
	}
	else
	{
		RangeVar *r;

		r = makeRangeVar(get_graph_path(), typname, -1);
		r->inhOpt = INH_YES;
		edge = (Node *) r;
	}

	if (vid != NULL)
	{
		ColumnRef  *begin;
		A_Expr	   *vidcond;

		if (crel->direction == CYPHER_REL_DIR_LEFT)
		{
			begin = makeNode(ColumnRef);
			begin->fields = list_make1(makeString(AG_END_ID));
			begin->location = -1;
		}
		else
		{
			begin = makeNode(ColumnRef);
			begin->fields = list_make1(makeString(AG_START_ID));
			begin->location = -1;
		}
		vidcond = makeSimpleA_Expr(AEXPR_OP, "=", (Node *) begin, vid, -1);
		where_args = lappend(where_args, vidcond);
	}

	/* TODO: cannot see properties of future vertices */
	if (crel->prop_map != NULL)
	{
		ColumnRef  *prop;
		A_Expr	   *propcond;

		prop = makeNode(ColumnRef);
		prop->fields = list_make1(makeString(AG_ELEM_PROP_MAP));
		prop->location = -1;
		propcond = makeSimpleA_Expr(AEXPR_OP, "@>", (Node *) prop,
									crel->prop_map, -1);
		where_args = lappend(where_args, propcond);
	}

	sel = makeNode(SelectStmt);
	sel->targetList = list_make2(end, level);
	if (start_out)
	{
		sel->targetList = lappend(sel->targetList,
								  makeSimpleResTarget(AG_START_ID, NULL));
	}
	if (path_out)
	{
		ColumnRef *id;
		A_ArrayExpr *patharr;
		ResTarget *path;

		id = makeNode(ColumnRef);
		id->fields = list_make1(makeString(AG_ELEM_LOCAL_ID));
		id->location = -1;
		patharr = makeNode(A_ArrayExpr);
		patharr->elements = list_make1(id);
		patharr->location = -1;
		path = makeResTarget((Node *) patharr, NULL);
		sel->targetList = lappend(sel->targetList, path);
	}
	sel->fromClause = list_make1(edge);
	sel->whereClause = (Node *) makeBoolExpr(AND_EXPR, where_args, -1);

	return sel;
}

/*
 * -- CYPHER_REL_DIR_LEFT
 * SELECT _e.start, _vlr.end, level + 1, array_append(path, id)
 * FROM _vlr, `get_graph_path()`.`typname` AS _e
 * WHERE level < `indices->uidx` AND
 *       _e.end = _vlr.start AND
 *       array_position(path, id) IS NULL AND
 *       properties @> `crel->prop_map`
 *
 * -- CYPHER_REL_DIR_RIGHT
 * SELECT _vlr.start, _e.end, level + 1, array_append(path, id)
 * FROM _vlr, `get_graph_path()`.`typname` AS _e
 * WHERE level < `indices->uidx` AND
 *       _vlr.end = _e.start AND
 *       array_position(path, id) IS NULL AND
 *       properties @> `crel->prop_map`
 *
 * -- CYPHER_REL_DIR_NONE
 * SELECT _vlr.start, _e.end, level + 1, array_append(path, id)
 * FROM _vlr, `genEdgeUnionVLR(typname)` AS _e
 * WHERE level < `indices->uidx` AND
 *       _vlr.end = _e.start AND
 *       array_position(path, id) IS NULL AND
 *       properties @> `crel->prop_map`
 */
static SelectStmt *
genSelectRightVLR(ParseState *pstate, CypherRel *crel)
{
	char	   *typname;
	ResTarget  *start = NULL;
	ResTarget  *end;
	ColumnRef  *levelref;
	A_Expr	   *levelexpr;
	ResTarget  *level;
	RangeVar   *vlr;
	Node       *edge;
	ColumnRef  *prev;
	ColumnRef  *next;
	ColumnRef  *dist_end;
	List	   *where_args = NIL;
	A_Expr	   *joincond;
	SelectStmt *sel;
	List       *arrpos_args;
	FuncCall   *arrpos;
	NullTest   *dupcond;
	ColumnRef  *id;
	ColumnRef  *pathref;

	getCypherRelType(crel, &typname, NULL);

	if (crel->direction == CYPHER_REL_DIR_LEFT)
	{
		start = makeFieldsResTarget(
					list_make2(makeString(CYPHER_VLR_EDGE_ALIAS),
							   makeString(AG_START_ID)),
					NULL);
		end = makeFieldsResTarget(list_make2(makeString(CYPHER_VLR_WITH_ALIAS),
											 makeString(VLR_COLNAME_END)),
								  NULL);
	}
	else
	{
		start = makeFieldsResTarget(
					list_make2(makeString(CYPHER_VLR_WITH_ALIAS),
							   makeString(VLR_COLNAME_START)),
					NULL);
		end = makeFieldsResTarget(list_make2(makeString(CYPHER_VLR_EDGE_ALIAS),
											 makeString(AG_END_ID)),
								  NULL);
	}

	levelref = makeNode(ColumnRef);
	levelref->fields = list_make1(makeString(VLR_COLNAME_LEVEL));
	levelref->location = -1;
	levelexpr = makeSimpleA_Expr(AEXPR_OP, "+", (Node *) levelref,
								 (Node *) makeIntConst(1), -1);
	level = makeResTarget((Node *) levelexpr, NULL);

	vlr = makeRangeVar(NULL, CYPHER_VLR_WITH_ALIAS, -1);

	if (crel->direction == CYPHER_REL_DIR_NONE)
	{
		RangeSubselect *sub;

		sub = genEdgeUnionVLR(typname);
		sub->alias = makeAliasNoDup(CYPHER_VLR_EDGE_ALIAS, NIL);
		edge = (Node *) sub;
	}
	else
	{
		RangeVar *r;

		r = makeRangeVar(get_graph_path(), typname, -1);
		r->alias = makeAliasNoDup(CYPHER_VLR_EDGE_ALIAS, NIL);
		r->inhOpt = INH_YES;
		edge = (Node *) r;
	}

	if (crel->direction == CYPHER_REL_DIR_LEFT)
	{
		prev = makeNode(ColumnRef);
		prev->fields = list_make2(makeString(CYPHER_VLR_WITH_ALIAS),
								  makeString(VLR_COLNAME_START));
		prev->location = -1;

		next = makeNode(ColumnRef);
		next->fields = list_make2(makeString(CYPHER_VLR_EDGE_ALIAS),
								  makeString(AG_END_ID));
		next->location = -1;
	}
	else
	{
		prev = makeNode(ColumnRef);
		prev->fields = list_make2(makeString(CYPHER_VLR_WITH_ALIAS),
								  makeString(VLR_COLNAME_END));
		prev->location = -1;

		next = makeNode(ColumnRef);
		next->fields = list_make2(makeString(CYPHER_VLR_EDGE_ALIAS),
								  makeString(AG_START_ID));
		next->location = -1;
	}

	joincond = makeSimpleA_Expr(AEXPR_OP, "=", (Node *) prev, (Node *) next,
								-1);
	pathref = makeNode(ColumnRef);
	pathref->fields = list_make1(makeString(VLR_COLNAME_PATH));
	pathref->location = -1;
	id = makeNode(ColumnRef);
	id->fields = list_make1(makeString(AG_ELEM_LOCAL_ID));
	id->location = -1;
	where_args = lappend(where_args, joincond);

	arrpos_args = list_make2(pathref, id);
	arrpos = makeFuncCall(list_make1(makeString("array_position")),
						  arrpos_args, -1);
	dupcond = makeNode(NullTest);
	dupcond->arg = (Expr *) arrpos;
	dupcond->nulltesttype = IS_NULL;
	dupcond->location = -1;
	where_args = lappend(where_args, dupcond);

	/* TODO: cannot see properties of future vertices */
	if (crel->prop_map != NULL)
	{
		ColumnRef  *prop;
		A_Expr	   *propcond;

		prop = makeNode(ColumnRef);
		prop->fields = list_make1(makeString(AG_ELEM_PROP_MAP));
		prop->location = -1;
		propcond = makeSimpleA_Expr(AEXPR_OP, "@>", (Node *) prop,
									crel->prop_map, -1);
		where_args = lappend(where_args, propcond);
	}

	sel = makeNode(SelectStmt);
	sel->targetList = list_make2(end, level);
	if (pstate->p_last_edge != NULL)
	{
		sel->targetList = lappend(sel->targetList, start);
	}
	if (true)
	{
		FuncCall   *pathexpr;
		ResTarget  *path;

		pathexpr = makeFuncCall(list_make1(makeString("array_append")),
								list_make2(pathref, id), -1);
		path = makeResTarget((Node *) pathexpr, NULL);
		sel->targetList = lappend(sel->targetList, path);
	}

	sel->fromClause = list_make2(vlr, edge);
	sel->whereClause = (Node *) makeBoolExpr(AND_EXPR, where_args, -1);
	dist_end = makeNode(ColumnRef);
	dist_end->fields = list_make1(makeString(VLR_COLNAME_END));
	dist_end->location = -1;
	sel->distinctClause = list_make1(dist_end);

	return sel;
}

/*
 * SELECT tableoid, ctid, id, properties, start, "end"
 * FROM `get_graph_path()`.`edge_label`
 * UNION
 * SELECT tableoid, ctid, id, properties, "end" as start, start as "end"
 * FROM `get_graph_path()`.`edge_label`
 */
static RangeSubselect *
genEdgeUnionVLR(char *edge_label)
{
	ResTarget  *tableoid;
	ResTarget  *ctid;
	ResTarget  *id;
	ResTarget  *prop_map;
	RangeVar   *r;
	SelectStmt *lsel;
	SelectStmt *rsel;
	SelectStmt *u;
	RangeSubselect *sub;

	tableoid = makeSimpleResTarget("tableoid", NULL);
	ctid = makeSimpleResTarget("ctid", NULL);
	id = makeSimpleResTarget(AG_ELEM_LOCAL_ID, NULL);
	prop_map = makeSimpleResTarget(AG_ELEM_PROP_MAP, NULL);

	r = makeRangeVar(get_graph_path(), edge_label, -1);
	r->inhOpt = INH_YES;

	lsel = makeNode(SelectStmt);
	lsel->targetList = list_make4(tableoid, ctid, id, prop_map);
	lsel->fromClause = list_make1(r);

	rsel = copyObject(lsel);

	lsel->targetList = lappend(lsel->targetList,
							   makeSimpleResTarget(AG_START_ID, NULL));
	lsel->targetList = lappend(lsel->targetList,
							   makeSimpleResTarget(AG_END_ID, NULL));

	rsel->targetList = lappend(rsel->targetList,
							   makeSimpleResTarget(AG_END_ID, AG_START_ID));
	rsel->targetList = lappend(rsel->targetList,
							   makeSimpleResTarget(AG_START_ID, AG_END_ID));

	u = makeNode(SelectStmt);
	u->op = SETOP_UNION;
	u->all = true;
	u->larg = lsel;
	u->rarg = rsel;

	sub = makeNode(RangeSubselect);
	sub->subquery = (Node *) u;

	return sub;
}

static SelectStmt *
genSelectWithVLR(ParseState *pstate, CypherRel *crel, WithClause *with)
{
	A_Indices  *indices = (A_Indices *) crel->varlen;
	ResTarget  *start = NULL;
	ResTarget  *end;
	RangeVar   *vlr;
	SelectStmt *sel;
	Node	   *lidx = NULL;

	if (crel->direction == CYPHER_REL_DIR_NONE)
	{
		start = makeSimpleResTarget(VLR_COLNAME_START, EDGE_UNION_START_ID);
		end = makeSimpleResTarget(VLR_COLNAME_END, EDGE_UNION_END_ID);
	}
	else
	{
		start = makeSimpleResTarget(VLR_COLNAME_START, AG_START_ID);
		end = makeSimpleResTarget(VLR_COLNAME_END, AG_END_ID);
	}

	vlr = makeRangeVar(NULL, CYPHER_VLR_WITH_ALIAS, -1);

	sel = makeNode(SelectStmt);
	sel->targetList = list_make1(end);
	if (pstate->p_last_edge != NULL)
	{
		sel->targetList = lappend(sel->targetList, start);
	}
	if (getCypherName(crel->variable) != NULL)
	{
		ResTarget *path;
		path = makeSimpleResTarget(VLR_COLNAME_PATH, NULL);
		sel->targetList = lappend(sel->targetList, path);
	}
	sel->fromClause = list_make1(vlr);

	if (((A_Const *) indices->lidx)->val.val.ival > 1)
		lidx = indices->lidx;

	if (lidx != NULL)
	{
		ColumnRef *level;

		level = makeNode(ColumnRef);
		level->fields = list_make1(makeString(VLR_COLNAME_LEVEL));
		level->location = -1;

		sel->whereClause = (Node *) makeSimpleA_Expr(AEXPR_OP, ">=",
													 (Node *) level, lidx, -1);
	}

	sel->withClause = with;

	return sel;
}

static RangeTblEntry *
transformVLRtoRTE(ParseState *pstate, SelectStmt *vlr, Alias *alias)
{
	ParseNamespaceItem *nsitem = NULL;
	Query	   *qry;
	RangeTblEntry *rte;

	Assert(!pstate->p_lateral_active);
	Assert(pstate->p_expr_kind == EXPR_KIND_NONE);

	/* make the RTE temporarily visible */
	if (pstate->p_vlr_initial_rte != NULL)
	{
		nsitem = findNamespaceItemForRTE(pstate, pstate->p_vlr_initial_rte);
		Assert(nsitem != NULL);

		nsitem->p_rel_visible = true;
	}

	pstate->p_lateral_active = true;
	pstate->p_expr_kind = EXPR_KIND_FROM_SUBSELECT;

	qry = parse_sub_analyze((Node *) vlr, pstate, NULL,
							isLockedRefname(pstate, alias->aliasname));
	Assert(qry->commandType == CMD_SELECT);

	pstate->p_lateral_active = false;
	pstate->p_expr_kind = EXPR_KIND_NONE;

	if (nsitem != NULL)
		nsitem->p_rel_visible = false;

	rte = addRangeTableEntryForSubquery(pstate, qry, alias, true, true);
	addRTEtoJoinlist(pstate, rte, false);

	return rte;
}

static bool
isZeroLengthVLR(CypherRel *crel)
{
	A_Indices  *indices;
	A_Const	   *lidx;

	if (crel == NULL)
		return false;

	if (crel->varlen == NULL)
		return false;

	indices = (A_Indices *) crel->varlen;
	lidx = (A_Const *) indices->lidx;
	return (lidx->val.val.ival == 0);
}

static void
getCypherRelType(CypherRel *crel, char **typname, int *typloc)
{
	if (crel->types == NIL)
	{
		*typname = AG_EDGE;
		if (typloc != NULL)
			*typloc = -1;
	}
	else
	{
		Node *type;

		if (list_length(crel->types) > 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("multiple types for relationship not supported")));

		type = linitial(crel->types);

		*typname = getCypherName(type);
		if (typloc != NULL)
			*typloc = getCypherNameLoc(type);
	}
}

static Node *
addQualRelPath(ParseState *pstate, Node *qual, CypherRel *prev_crel,
			   RangeTblEntry *prev_edge, CypherRel *crel, RangeTblEntry *edge)
{
	Node	   *prev_vid;
	Node	   *vid;

	/*
	 * NOTE: If `crel` is VLR and a node between `prev_crel` and `crel` is
	 *       either a placeholder or a new future vertex,
	 *       the initial vid of `crel` is `prev_vid` already.
	 *       Currently, just add kind of duplicate qual anyway.
	 */

	prev_vid = getColumnVar(pstate, prev_edge, getEdgeColname(prev_crel, true));
	vid = getColumnVar(pstate, edge, getEdgeColname(crel, false));

	qual = qualAndExpr(qual,
					   (Node *) make_op(pstate, list_make1(makeString("=")),
										prev_vid, vid, -1));

	return qual;
}

static Node *
addQualNodeIn(ParseState *pstate, Node *qual, Node *vertex, CypherRel *crel,
			  RangeTblEntry *edge, bool prev)
{
	Node	   *id;
	Node	   *vid;

	/* `vertex` is just a placeholder for relationships */
	if (vertex == NULL)
		return qual;

	if (isFutureVertexExpr(vertex))
	{
		setFutureVertexExprId(pstate, vertex, crel, edge, prev);
		return qual;
	}

	/* already done in transformMatchVLR() */
	if (crel->varlen != NULL && !prev)
		return qual;

	if (IsA(vertex, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) vertex;

		Assert(rte->rtekind == RTE_RELATION);

		id = getColumnVar(pstate, rte, AG_ELEM_LOCAL_ID);
	}
	else
	{
		TargetEntry *te = (TargetEntry *) vertex;

		AssertArg(IsA(vertex, TargetEntry));

		id = getExprField(te->expr, AG_ELEM_ID);
	}
	vid = getColumnVar(pstate, edge, getEdgeColname(crel, prev));

	qual = qualAndExpr(qual,
					   (Node *) make_op(pstate, list_make1(makeString("=")),
										id, vid, -1));

	return qual;
}

static char *
getEdgeColname(CypherRel *crel, bool prev)
{
	if (prev)
	{
		if (crel->direction == CYPHER_REL_DIR_NONE)
			return EDGE_UNION_END_ID;
		else if (crel->direction == CYPHER_REL_DIR_LEFT)
			return AG_START_ID;
		else
			return AG_END_ID;
	}
	else
	{
		if (crel->direction == CYPHER_REL_DIR_NONE)
			return EDGE_UNION_START_ID;
		else if (crel->direction == CYPHER_REL_DIR_LEFT)
			return AG_END_ID;
		else
			return AG_START_ID;
	}
}

static bool
isFutureVertexExpr(Node *vertex)
{
	TargetEntry *te;
	RowExpr *row;

	AssertArg(vertex != NULL);

	if (!IsA(vertex, TargetEntry))
		return false;

	te = (TargetEntry *) vertex;
	if (!IsA(te->expr, RowExpr))
		return false;

	row = (RowExpr *) te->expr;

	/* a Const node representing a NULL */
	return IsA(lsecond(row->args), Const);
}

static void
setFutureVertexExprId(ParseState *pstate, Node *vertex, CypherRel *crel,
					  RangeTblEntry *edge, bool prev)
{
	TargetEntry *te = (TargetEntry *) vertex;
	RowExpr	   *row;
	Node	   *vid;

	row = (RowExpr *) te->expr;
	vid = getColumnVar(pstate, edge, getEdgeColname(crel, prev));
	row->args = list_make2(vid, lsecond(row->args));
}

static Node *
addQualUniqueEdges(ParseState *pstate, Node *qual, List *ueids, List *ueidarrs)
{
	FuncCall   *arrpos;
	ListCell   *le1;
	ListCell   *lea1;

	arrpos = makeFuncCall(list_make1(makeString("array_position")), NIL, -1);

	foreach(le1, ueids)
	{
		Node	   *eid1 = lfirst(le1);
		ListCell   *le2;

		for_each_cell(le2, lnext(le1))
		{
			Node	   *eid2 = lfirst(le2);
			Expr	   *ne;

			ne = make_op(pstate, list_make1(makeString("<>")), eid1, eid2, -1);

			qual = qualAndExpr(qual, (Node *) ne);
		}

		foreach(lea1, ueidarrs)
		{
			Node	   *eidarr = lfirst(lea1);
			Node	   *arg;
			NullTest   *dupcond;

			arg = ParseFuncOrColumn(pstate,
									list_make1(makeString("array_position")),
									list_make2(eidarr, eid1), arrpos, -1);

			dupcond = makeNode(NullTest);
			dupcond->arg = (Expr *) arg;
			dupcond->nulltesttype = IS_NULL;
			dupcond->argisrow = false;
			dupcond->location = -1;

			qual = qualAndExpr(qual, (Node *) dupcond);
		}
	}

	foreach(lea1, ueidarrs)
	{
		Node	   *eidarr1 = lfirst(lea1);
		ListCell   *lea2;

		for_each_cell(lea2, lnext(lea1))
		{
			Node	   *eidarr2 = lfirst(lea2);
			Node	   *overlap;
			Node	   *dupcond;

			overlap = ParseFuncOrColumn(pstate,
										list_make1(makeString("arrayoverlap")),
										list_make2(eidarr1, eidarr2), arrpos,
										-1);

			dupcond = (Node *) makeBoolExpr(NOT_EXPR, list_make1(overlap), -1);

			qual = qualAndExpr(qual, dupcond);
		}
	}

	return qual;
}

static void
addElemQual(ParseState *pstate, AttrNumber varattno, Node *prop_constr)
{
	ElemQual *eq;

	if (prop_constr == NULL)
		return;

	eq = palloc(sizeof(*eq));
	eq->varno = InvalidAttrNumber;
	eq->varattno = varattno;
	eq->prop_constr = prop_constr;

	pstate->p_elem_quals = lappend(pstate->p_elem_quals, eq);
}

static void
adjustElemQuals(List *elem_quals, RangeTblEntry *rte, int rtindex)
{
	ListCell *le;

	AssertArg(rte->rtekind == RTE_SUBQUERY);

	foreach(le, elem_quals)
	{
		ElemQual *eq = lfirst(le);

		eq->varno = rtindex;
	}
}

static Node *
transformElemQuals(ParseState *pstate, Node *qual)
{
	ListCell *le;

	foreach(le, pstate->p_elem_quals)
	{
		ElemQual   *eq = lfirst(le);
		RangeTblEntry *rte;
		Var		   *var;
		Node	   *prop_map;
		Node	   *prop_constr;
		Expr	   *expr;

		rte = GetRTEByRangeTablePosn(pstate, eq->varno, 0);
		var = make_var(pstate, rte, eq->varattno, -1);
		/* skip markVarForSelectPriv() because `rte` is RTE_SUBQUERY */

		prop_map = getExprField((Expr *) var, AG_ELEM_PROP_MAP);
		prop_constr = transformPropMap(pstate, eq->prop_constr,
									   EXPR_KIND_WHERE);
		expr = make_op(pstate, list_make1(makeString("@>")),
					   prop_map, prop_constr, -1);

		qual = qualAndExpr(qual, (Node *) expr);
	}

	pstate->p_elem_quals = NIL;
	return qual;
}

static void
addFutureVertex(ParseState *pstate, AttrNumber varattno, char *labname)
{
	FutureVertex *fv;

	fv = palloc(sizeof(*fv));
	fv->varno = InvalidAttrNumber;
	fv->varattno = varattno;
	fv->labname = labname;
	fv->nullable = pstate->p_is_optional_match;
	fv->expr = NULL;

	pstate->p_future_vertices = lappend(pstate->p_future_vertices, fv);
}

static FutureVertex *
findFutureVertex(ParseState *pstate, Index varno, AttrNumber varattno,
				 int sublevels_up)
{
	ListCell *le;

	while (sublevels_up-- > 0)
	{
		pstate = pstate->parentParseState;
		Assert(pstate != NULL);
	}

	foreach(le, pstate->p_future_vertices)
	{
		FutureVertex *fv = lfirst(le);

		if (fv->varno == varno && fv->varattno == varattno)
			return fv;
	}

	return NULL;
}

static void
adjustFutureVertices(List *future_vertices, RangeTblEntry *rte, int rtindex)
{
	ListCell *prev;
	ListCell *le;
	ListCell *next;

	AssertArg(rte->rtekind == RTE_SUBQUERY);

	prev = NULL;
	for (le = list_head(future_vertices); le != NULL; le = next)
	{
		FutureVertex *fv = lfirst(le);
		bool		found;
		ListCell   *lt;

		next = lnext(le);

		/* set `varno` of new future vertex to its `rtindex` */
		if (fv->varno == InvalidAttrNumber)
		{
			fv->varno = rtindex;

			prev = le;
			continue;
		}

		found = false;
		foreach(lt, rte->subquery->targetList)
		{
			TargetEntry *te = lfirst(lt);
			Var *var;

			if (exprType((Node *) te->expr) != VERTEXOID)
				continue;

			/*
			 * skip all forms of vertex (e.g. `(id, properties)::vertex`)
			 * except variables of vertex
			 */
			if (!IsA(te->expr, Var))
				continue;

			var = (Var *) te->expr;
			if (var->varno == fv->varno && var->varattno == fv->varattno &&
				var->varlevelsup == 0)
			{
				fv->varno = rtindex;

				/*
				 * `te->resno` should always be equal to the item's
				 * ordinal position (counting from 1)
				 */
				fv->varattno = te->resno;

				found = true;
			}
		}

		if (!found)
			future_vertices = list_delete_cell(future_vertices, le, prev);
		else
			prev = le;
	}
}

static Node *
resolve_future_vertex(ParseState *pstate, Node *node, int flags)
{
	resolve_future_vertex_context ctx;

	ctx.pstate = pstate;
	ctx.flags = flags;
	ctx.sublevels_up = 0;

	return resolve_future_vertex_mutator(node, &ctx);
}

static Node *
resolve_future_vertex_mutator(Node *node, resolve_future_vertex_context *ctx)
{
	Var *var;

	if (node == NULL)
		return NULL;

	if (IsA(node, Aggref))
	{
		Aggref	   *agg = (Aggref *) node;
		int			agglevelsup = (int) agg->agglevelsup;

		if (agglevelsup == ctx->sublevels_up)
		{
			ListCell *la;

			agg->aggdirectargs = (List *) resolve_future_vertex_mutator(
												(Node *) agg->aggdirectargs,
												ctx);

			foreach(la, agg->args)
			{
				TargetEntry *arg = lfirst(la);

				if (!IsA(arg->expr, Var))
					arg->expr = (Expr *) resolve_future_vertex_mutator(
															(Node *) arg->expr,
															ctx);
			}

			return node;
		}

		if (agglevelsup > ctx->sublevels_up)
			return node;

		/* fall-through */
	}

	if (IsA(node, FieldSelect))
	{
		FieldSelect *fselect = (FieldSelect *) node;

		if (IsA(fselect->arg, Var))
		{
			var = (Var *) fselect->arg;

			/* TODO: use Anum_vertex_id */
			if ((int) var->varlevelsup == ctx->sublevels_up &&
				exprType((Node *) var) == VERTEXOID &&
				fselect->fieldnum == 1)
				return node;
		}

		/* fall-through */
	}

	if (IsA(node, Var))
	{
		FutureVertex *fv;
		Var *newvar;

		var = (Var *) node;

		if ((int) var->varlevelsup != ctx->sublevels_up)
			return node;

		if (exprType(node) != VERTEXOID)
			return node;

		fv = findFutureVertex(ctx->pstate, var->varno, var->varattno, 0);
		if (fv == NULL)
			return node;

		if (fv->expr == NULL)
		{
			if (ctx->flags & FVR_DONT_RESOLVE)
				return node;

			resolveFutureVertex(ctx->pstate, fv,
								(ctx->flags & FVR_IGNORE_NULLABLE));
		}

		newvar = copyObject(fv->expr);
		if (ctx->flags & FVR_PRESERVE_VAR_REF)
		{
			/* XXX: is this OK? */
			newvar->varno = fv->varno;
			newvar->varattno = fv->varattno;
		}
		newvar->varlevelsup = ctx->sublevels_up;

		return (Node *) newvar;
	}

	if (IsA(node, Query))
	{
		Query *newnode;

		ctx->sublevels_up++;
		newnode = query_tree_mutator((Query *) node,
									 resolve_future_vertex_mutator, ctx, 0);
		ctx->sublevels_up--;

		return (Node *) newnode;
	}

	return expression_tree_mutator(node, resolve_future_vertex_mutator, ctx);
}

static void
resolveFutureVertex(ParseState *pstate, FutureVertex *fv, bool ignore_nullable)
{
	RangeTblEntry *fv_rte;
	TargetEntry *fv_te;
	Var		   *fv_var;
	Node	   *fv_id;
	RangeTblEntry *rte;
	Node	   *vertex;
	FuncCall   *sel_id;
	Node	   *id;
	Node	   *qual;

	AssertArg(fv->expr == NULL);

	fv_rte = GetRTEByRangeTablePosn(pstate, fv->varno, 0);
	Assert(fv_rte->rtekind == RTE_SUBQUERY);

	fv_te = get_tle_by_resno(fv_rte->subquery->targetList, fv->varattno);
	Assert(fv_te != NULL);

	fv_var = make_var(pstate, fv_rte, fv->varattno, -1);
	fv_id = getExprField((Expr *) fv_var, AG_ELEM_ID);

	/*
	 * `p_cols_visible` of previous RTE must be set to allow `rte` to see
	 * columns of the previous RTE by their name
	 */
	rte = makeVertexRTE(pstate, fv_te->resname, fv->labname);

	vertex = getColumnVar(pstate, rte, rte->eref->aliasname);

	sel_id = makeFuncCall(list_make1(makeString(AG_ELEM_ID)), NIL, -1);
	id = ParseFuncOrColumn(pstate, sel_id->funcname, list_make1(vertex),
						   sel_id, -1);

	qual = (Node *) make_op(pstate, list_make1(makeString("=")), fv_id, id, -1);

	if (ignore_nullable)
	{
		addRTEtoJoinlist(pstate, rte, false);

		pstate->p_resolved_qual = qualAndExpr(pstate->p_resolved_qual, qual);
	}
	else
	{
		JoinType	jointype = (fv->nullable ? JOIN_LEFT : JOIN_INNER);
		Node	   *l_jt;
		int			l_rtindex;
		RangeTblEntry *l_rte;
		Alias	   *alias;

		l_jt = llast(pstate->p_joinlist);
		if (IsA(l_jt, RangeTblRef))
		{
			l_rtindex = ((RangeTblRef *) l_jt)->rtindex;
		}
		else
		{
			Assert(IsA(l_jt, JoinExpr));
			l_rtindex = ((JoinExpr *) l_jt)->rtindex;
		}
		l_rte = rt_fetch(l_rtindex, pstate->p_rtable);

		alias = makeAliasNoDup(CYPHER_SUBQUERY_ALIAS, NIL);
		incrementalJoinRTEs(pstate, jointype, l_rte, rte, qual, alias);
	}

	/* modify `fv->expr` to the actual vertex */
	fv->expr = (Expr *) vertex;
}

static RangeTblEntry *
makeVertexRTE(ParseState *parentParseState, char *varname, char *labname)
{
	Alias	   *alias;
	ParseState *pstate;
	Query	   *qry;
	RangeVar   *r;
	RangeTblEntry *rte;
	TargetEntry *te;

	Assert(parentParseState->p_expr_kind == EXPR_KIND_NONE);
	parentParseState->p_expr_kind = EXPR_KIND_FROM_SUBSELECT;

	alias = makeAlias(varname, NIL);

	pstate = make_parsestate(parentParseState);
	pstate->p_locked_from_parent = isLockedRefname(pstate, alias->aliasname);

	qry = makeNode(Query);
	qry->commandType = CMD_SELECT;

	r = makeRangeVar(get_graph_path(), labname, -1);

	rte = addRangeTableEntry(pstate, r, alias, true, true);
	addRTEtoJoinlist(pstate, rte, false);

	te = makeTargetEntry((Expr *) makeVertexExpr(pstate, rte, -1),
						 (AttrNumber) pstate->p_next_resno++,
						 alias->aliasname,
						 false);

	qry->targetList = list_make1(te);
	markTargetListOrigins(pstate, qry->targetList);

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	assign_query_collations(pstate, qry);

	parentParseState->p_expr_kind = EXPR_KIND_NONE;

	return addRangeTableEntryForSubquery(parentParseState, qry, alias, false,
										 true);
}

static List *
removeResolvedFutureVertices(List *future_vertices)
{
	ListCell   *prev;
	ListCell   *le;
	ListCell   *next;

	prev = NULL;
	for (le = list_head(future_vertices); le != NULL; le = next)
	{
		FutureVertex *fv = lfirst(le);

		next = lnext(le);
		if (fv->expr == NULL)
			prev = le;
		else
			future_vertices = list_delete_cell(future_vertices, le, prev);
	}

	return future_vertices;
}

static List *
transformCreatePattern(ParseState *pstate, List *pattern, List **targetList)
{
	List	   *graphPattern = NIL;
	ListCell   *lp;

	foreach(lp, pattern)
	{
		CypherPath *p = lfirst(lp);
		char	   *pathname = getCypherName(p->variable);
		int			pathloc = getCypherNameLoc(p->variable);
		List	   *gchain = NIL;
		GraphPath  *gpath;
		ListCell   *le;

		if (findTarget(*targetList, pathname) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_ALIAS),
					 errmsg("duplicate variable \"%s\"", pathname),
					 parser_errposition(pstate, pathloc)));

		foreach(le, p->chain)
		{
			Node *elem = lfirst(le);

			if (IsA(elem, CypherNode))
			{
				CypherNode *cnode = (CypherNode *) elem;
				GraphVertex *gvertex;

				gvertex = transformCreateNode(pstate, cnode, targetList);

				if (!gvertex->create && list_length(p->chain) <= 1)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("there must be at least one relationship"),
							 parser_errposition(pstate,
										getCypherNameLoc(cnode->variable))));

				gchain = lappend(gchain, gvertex);
			}
			else
			{
				CypherRel  *crel = (CypherRel *) elem;
				GraphEdge  *gedge;

				Assert(IsA(elem, CypherRel));

				gedge = transformCreateRel(pstate, crel, targetList);

				gchain = lappend(gchain, gedge);
			}
		}

		if (pathname != NULL)
		{
			Const *dummy;
			TargetEntry *te;

			dummy = makeNullConst(GRAPHPATHOID, -1, InvalidOid);
			te = makeTargetEntry((Expr *) dummy,
								 (AttrNumber) pstate->p_next_resno++,
								 pathname,
								 false);

			*targetList = lappend(*targetList, te);
		}

		gpath = makeNode(GraphPath);
		if (pathname != NULL)
			gpath->variable = pstrdup(pathname);
		gpath->chain = gchain;

		graphPattern = lappend(graphPattern, gpath);
	}

	return graphPattern;
}

static GraphVertex *
transformCreateNode(ParseState *pstate, CypherNode *cnode, List **targetList)
{
	char	   *varname = getCypherName(cnode->variable);
	int			varloc = getCypherNameLoc(cnode->variable);
	TargetEntry *te;
	bool		create;
	Node	   *prop_map = NULL;
	GraphVertex *gvertex;

	te = findTarget(*targetList, varname);
	if (te != NULL &&
		(exprType((Node *) te->expr) != VERTEXOID || !isNodeForRef(cnode)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_ALIAS),
				 errmsg("duplicate variable \"%s\"", varname),
				 parser_errposition(pstate, varloc)));

	create = (te == NULL);

	if (create)
	{
		if (varname != NULL)
		{
			Const *dummy;

			/*
			 * Create a room for a newly created vertex.
			 * This dummy value will be replaced with the vertex
			 * in ExecCypherCreate().
			 */

			dummy = makeNullConst(VERTEXOID, -1, InvalidOid);
			te = makeTargetEntry((Expr *) dummy,
								 (AttrNumber) pstate->p_next_resno++,
								 varname,
								 false);

			*targetList = lappend(*targetList, te);
		}

		if (cnode->prop_map != NULL)
			prop_map = transformPropMap(pstate, cnode->prop_map,
										EXPR_KIND_INSERT_TARGET);
	}

	gvertex = makeNode(GraphVertex);
	if (varname != NULL)
		gvertex->variable = pstrdup(varname);
	if (cnode->label != NULL)
		gvertex->label = pstrdup(getCypherName(cnode->label));
	gvertex->prop_map = prop_map;
	gvertex->create = create;

	return gvertex;
}

static GraphEdge *
transformCreateRel(ParseState *pstate, CypherRel *crel, List **targetList)
{
	char	   *varname;
	GraphEdge  *gedge;

	if (crel->direction == CYPHER_REL_DIR_NONE)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("only directed relationships are allowed in CREATE")));

	if (list_length(crel->types) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("only one relationship type is allowed for CREATE")));

	if (crel->varlen != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("variable length relationship is not allowed for CREATE")));

	varname = getCypherName(crel->variable);

	/*
	 * All relationships must be unique and We cannot reference an edge
	 * from the previous clause in CREATE clause.
	 */
	if (findTarget(*targetList, varname) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_ALIAS),
				 errmsg("duplicate variable \"%s\"", varname),
				 parser_errposition(pstate, getCypherNameLoc(crel->variable))));

	if (varname != NULL)
	{
		TargetEntry *te;

		te = makeTargetEntry((Expr *) makeNullConst(EDGEOID, -1, InvalidOid),
							 (AttrNumber) pstate->p_next_resno++,
							 varname,
							 false);

		*targetList = lappend(*targetList, te);
	}

	gedge = makeNode(GraphEdge);
	switch (crel->direction)
	{
		case CYPHER_REL_DIR_LEFT:
			gedge->direction = GRAPH_EDGE_DIR_LEFT;
			break;
		case CYPHER_REL_DIR_RIGHT:
			gedge->direction = GRAPH_EDGE_DIR_RIGHT;
			break;
		case CYPHER_REL_DIR_NONE:
		default:
			Assert(!"invalid direction");
	}
	if (varname != NULL)
		gedge->variable = pstrdup(varname);
	gedge->label = pstrdup(getCypherName(linitial(crel->types)));
	if (crel->prop_map != NULL)
		gedge->prop_map = transformPropMap(pstate, crel->prop_map,
										   EXPR_KIND_INSERT_TARGET);

	return gedge;
}

static List *
transformSetPropList(ParseState *pstate, RangeTblEntry *rte, List *items)
{
	List	   *sps = NIL;
	ListCell   *li;

	foreach(li, items)
	{
		CypherSetProp *sp = lfirst(li);

		sps = lappend(sps, transformSetProp(pstate, rte, sp));
	}

	return sps;
}

static GraphSetProp *
transformSetProp(ParseState *pstate, RangeTblEntry *rte, CypherSetProp *sp)
{
	Node	   *node;
	List	   *inds;
	Node	   *elem;
	List	   *pathelems = NIL;
	ListCell   *lf;
	Node	   *expr;
	Oid			exprtype;
	Node	   *cexpr;
	GraphSetProp *gsp;

	if (!IsA(sp->prop, ColumnRef) && !IsA(sp->prop, A_Indirection))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("only variable or property is valid for SET target")));

	if (IsA(sp->prop, A_Indirection))
	{
		A_Indirection *ind = (A_Indirection *) sp->prop;

		node = ind->arg;
		inds = ind->indirection;
	}
	else
	{
		node = sp->prop;
		inds = NIL;
	}

	if (IsA(node, ColumnRef))
	{
		ColumnRef  *cref = (ColumnRef *) node;
		char	   *varname = strVal(linitial(cref->fields));

		elem = getColumnVar(pstate, rte, varname);

		if (list_length(cref->fields) > 1)
		{
			for_each_cell(lf, lnext(list_head(cref->fields)))
			{
				pathelems = lappend(pathelems,
									transformJsonKey(pstate, lfirst(lf)));
			}
		}
	}
	else
	{
		Oid elemtype;

		elem = transformExpr(pstate, node, EXPR_KIND_UPDATE_TARGET);

		elemtype = exprType(elem);
		if (elemtype != VERTEXOID && elemtype != EDGEOID)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("node or relationship is expected"),
					 parser_errposition(pstate, exprLocation(elem))));
	}

	if (inds != NIL)
	{
		foreach(lf, inds)
		{
			pathelems = lappend(pathelems,
								transformJsonKey(pstate, lfirst(lf)));
		}
	}

	expr = transformExpr(pstate, sp->expr, EXPR_KIND_UPDATE_SOURCE);
	expr = resolve_future_vertex(pstate, expr, FVR_PRESERVE_VAR_REF);
	exprtype = exprType(expr);
	cexpr = coerce_to_target_type(pstate, expr, exprtype, JSONBOID, -1,
								  COERCION_ASSIGNMENT, COERCE_IMPLICIT_CAST,
								  -1);
	if (cexpr == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("expression must be of type jsonb but %s",
						format_type_be(exprtype)),
				 parser_errposition(pstate, exprLocation(expr))));

	gsp = makeNode(GraphSetProp);
	gsp->elem = resolve_future_vertex(pstate, elem, FVR_PRESERVE_VAR_REF);
	if (pathelems != NIL)
	{
		gsp->path = makeArrayExpr(TEXTARRAYOID, TEXTOID, pathelems);
		gsp->path = resolve_future_vertex(pstate, gsp->path,
										  FVR_PRESERVE_VAR_REF);
	}
	gsp->expr = cexpr;

	return gsp;
}

static bool
isNodeForRef(CypherNode *cnode)
{
	return (getCypherName(cnode->variable) != NULL &&
			getCypherName(cnode->label) == NULL &&
			cnode->prop_map == NULL);
}

static Node *
transformPropMap(ParseState *pstate, Node *expr, ParseExprKind exprKind)
{
	Node *prop_map;

	prop_map = transformExpr(pstate, preprocessPropMap(expr), exprKind);
	if (exprType(prop_map) != JSONBOID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("property map must be jsonb type"),
				 parser_errposition(pstate, exprLocation(prop_map))));

	return resolve_future_vertex(pstate, prop_map, 0);
}

static Node *
preprocessPropMap(Node *expr)
{
	Node *result = expr;

	if (IsA(expr, A_Const))
	{
		A_Const *c = (A_Const *) expr;

		if (IsA(&c->val, String))
			result = (Node *) makeFuncCall(list_make1(makeString("jsonb_in")),
										   list_make1(expr), -1);
	}

	return result;
}

static RangeTblEntry *
transformClause(ParseState *pstate, Node *clause)
{
	Alias *alias;
	RangeTblEntry *rte;

	alias = makeAliasNoDup(CYPHER_SUBQUERY_ALIAS, NIL);
	rte = transformClauseImpl(pstate, clause, alias);
	addRTEtoJoinlist(pstate, rte, true);

	return rte;
}

static RangeTblEntry *
transformClauseImpl(ParseState *pstate, Node *clause, Alias *alias)
{
	ParseState *childParseState;
	Query	   *qry;
	List	   *future_vertices;
	RangeTblEntry *rte;
	int			rtindex;

	AssertArg(IsA(clause, CypherClause));

	Assert(pstate->p_expr_kind == EXPR_KIND_NONE);
	pstate->p_expr_kind = EXPR_KIND_FROM_SUBSELECT;

	childParseState = make_parsestate(pstate);
	childParseState->p_is_match_quals = pstate->p_is_match_quals;
	childParseState->p_is_optional_match = pstate->p_is_optional_match;

	qry = transformStmt(childParseState, clause);

	pstate->p_elem_quals = childParseState->p_elem_quals;
	future_vertices = childParseState->p_future_vertices;

	free_parsestate(childParseState);

	pstate->p_expr_kind = EXPR_KIND_NONE;

	if (!IsA(qry, Query) ||
		(qry->commandType != CMD_SELECT &&
		 qry->commandType != CMD_GRAPHWRITE) ||
		qry->utilityStmt != NULL)
		elog(ERROR, "unexpected command in previous clause");

	rte = addRangeTableEntryForSubquery(pstate, qry, alias,
										pstate->p_lateral_active, true);

	rtindex = RTERangeTablePosn(pstate, rte, NULL);

	adjustElemQuals(pstate->p_elem_quals, rte, rtindex);

	future_vertices = removeResolvedFutureVertices(future_vertices);
	adjustFutureVertices(future_vertices, rte, rtindex);
	pstate->p_future_vertices = list_concat(pstate->p_future_vertices,
											future_vertices);

	return rte;
}

static RangeTblEntry *
incrementalJoinRTEs(ParseState *pstate, JoinType jointype,
					RangeTblEntry *l_rte, RangeTblEntry *r_rte, Node *qual,
					Alias *alias)
{
	int			l_rtindex;
	ListCell   *le;
	Node	   *l_jt = NULL;
	RangeTblRef *r_rtr;
	ParseNamespaceItem *r_nsitem;
	List	   *res_colnames = NIL;
	List	   *res_colvars = NIL;
	JoinExpr   *j;
	RangeTblEntry *rte;
	int			i;
	ParseNamespaceItem *nsitem;

	/* find JOIN-subtree of `l_rte` */
	l_rtindex = RTERangeTablePosn(pstate, l_rte, NULL);
	foreach(le, pstate->p_joinlist)
	{
		Node	   *jt = lfirst(le);
		int			rtindex;

		if (IsA(jt, RangeTblRef))
		{
			rtindex = ((RangeTblRef *) jt)->rtindex;
		}
		else
		{
			Assert(IsA(jt, JoinExpr));
			rtindex = ((JoinExpr *) jt)->rtindex;
		}

		if (rtindex == l_rtindex)
			l_jt = jt;
	}
	Assert(l_jt != NULL);

	makeExtraFromRTE(pstate, r_rte, &r_rtr, &r_nsitem, false);

	j = makeNode(JoinExpr);
	j->jointype = jointype;
	j->larg = l_jt;
	j->rarg = (Node *) r_rtr;
	j->quals = qual;
	j->alias = alias;

	makeJoinResCols(pstate, l_rte, r_rte, &res_colnames, &res_colvars);
	rte = addRangeTableEntryForJoin(pstate, res_colnames, j->jointype,
									res_colvars, j->alias, true);
	j->rtindex = RTERangeTablePosn(pstate, rte, NULL);

	for (i = list_length(pstate->p_joinexprs) + 1; i < j->rtindex; i++)
		pstate->p_joinexprs = lappend(pstate->p_joinexprs, NULL);
	pstate->p_joinexprs = lappend(pstate->p_joinexprs, j);
	Assert(list_length(pstate->p_joinexprs) == j->rtindex);

	pstate->p_joinlist = list_delete_ptr(pstate->p_joinlist, l_jt);
	pstate->p_joinlist = lappend(pstate->p_joinlist, j);

	makeExtraFromRTE(pstate, rte, NULL, &nsitem, true);
	pstate->p_namespace = lappend(pstate->p_namespace, r_nsitem);
	pstate->p_namespace = lappend(pstate->p_namespace, nsitem);

	return rte;
}

static void
makeJoinResCols(ParseState *pstate, RangeTblEntry *l_rte, RangeTblEntry *r_rte,
				List **res_colnames, List **res_colvars)
{
	List	   *l_colnames;
	List	   *l_colvars;
	List	   *r_colnames;
	List	   *r_colvars;
	ListCell   *r_lname;
	ListCell   *r_lvar;
	List	   *colnames = NIL;
	List	   *colvars = NIL;

	expandRTE(l_rte, RTERangeTablePosn(pstate, l_rte, NULL), 0, -1, false,
			  &l_colnames, &l_colvars);
	expandRTE(r_rte, RTERangeTablePosn(pstate, r_rte, NULL), 0, -1, false,
			  &r_colnames, &r_colvars);

	*res_colnames = list_concat(*res_colnames, l_colnames);
	*res_colvars = list_concat(*res_colvars, l_colvars);

	forboth(r_lname, r_colnames, r_lvar, r_colvars)
	{
		char	   *r_colname = strVal(lfirst(r_lname));
		ListCell   *lname;
		ListCell   *lvar;
		Var		   *var = NULL;

		forboth(lname, *res_colnames, lvar, *res_colvars)
		{
			char *colname = strVal(lfirst(lname));

			if (strcmp(r_colname, colname) == 0)
			{
				var = lfirst(lvar);
				break;
			}
		}

		if (var == NULL)
		{
			colnames = lappend(colnames, lfirst(r_lname));
			colvars = lappend(colvars, lfirst(r_lvar));
		}
		else
		{
			Var		   *r_var = lfirst(r_lvar);
			Oid			vartype;
			Oid			r_vartype;

			vartype = exprType((Node *) var);
			r_vartype = exprType((Node *) r_var);
			if (vartype != r_vartype)
			{
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("variable type mismatch")));
			}
			if (vartype != VERTEXOID && vartype != EDGEOID)
			{
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("node or relationship is expected")));
			}
		}
	}

	*res_colnames = list_concat(*res_colnames, colnames);
	*res_colvars = list_concat(*res_colvars, colvars);
}

static void
addRTEtoJoinlist(ParseState *pstate, RangeTblEntry *rte, bool visible)
{
	RangeTblEntry *tmp;
	RangeTblRef *rtr;
	ParseNamespaceItem *nsitem;

	/*
	 * There should be no namespace conflicts because we check a variable
	 * (which becomes an alias) is duplicated. This check remains to prevent
	 * future programming error.
	 */
	tmp = findRTEfromNamespace(pstate, rte->eref->aliasname);
	if (tmp != NULL)
	{
		if (!(rte->rtekind == RTE_RELATION && rte->alias == NULL &&
			  tmp->rtekind == RTE_RELATION && tmp->alias == NULL &&
			  rte->relid != tmp->relid))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_ALIAS),
					 errmsg("variable \"%s\" specified more than once",
							rte->eref->aliasname)));
	}

	makeExtraFromRTE(pstate, rte, &rtr, &nsitem, visible);
	pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);
	pstate->p_namespace = lappend(pstate->p_namespace, nsitem);
}

static void
makeExtraFromRTE(ParseState *pstate, RangeTblEntry *rte, RangeTblRef **rtr,
				 ParseNamespaceItem **nsitem, bool visible)
{
	if (rtr != NULL)
	{
		RangeTblRef *_rtr;

		_rtr = makeNode(RangeTblRef);
		_rtr->rtindex = RTERangeTablePosn(pstate, rte, NULL);

		*rtr = _rtr;
	}

	if (nsitem != NULL)
	{
		ParseNamespaceItem *_nsitem;

		_nsitem = (ParseNamespaceItem *) palloc(sizeof(ParseNamespaceItem));
		_nsitem->p_rte = rte;
		_nsitem->p_rel_visible = visible;
		_nsitem->p_cols_visible = visible;
		_nsitem->p_lateral_only = false;
		_nsitem->p_lateral_ok = true;

		*nsitem = _nsitem;
	}
}

/* just find RTE of `refname` in the current namespace */
static RangeTblEntry *
findRTEfromNamespace(ParseState *pstate, char *refname)
{
	ListCell *lni;

	if (refname == NULL)
		return NULL;

	foreach(lni, pstate->p_namespace)
	{
		ParseNamespaceItem *nsitem = lfirst(lni);
		RangeTblEntry *rte = nsitem->p_rte;

		/* NOTE: skip all checks on `nsitem` */

		if (strcmp(rte->eref->aliasname, refname) == 0)
			return rte;
	}

	return NULL;
}

static ParseNamespaceItem *
findNamespaceItemForRTE(ParseState *pstate, RangeTblEntry *rte)
{
	ListCell *lni;

	foreach(lni, pstate->p_namespace)
	{
		ParseNamespaceItem *nsitem = lfirst(lni);

		if (nsitem->p_rte == rte)
			return nsitem;
	}

	return NULL;
}

static List *
makeTargetListFromRTE(ParseState *pstate, RangeTblEntry *rte)
{
	List	   *targetlist = NIL;
	int			rtindex;
	int			varattno;
	ListCell   *ln;
	ListCell   *lt;

	AssertArg(rte->rtekind == RTE_SUBQUERY);

	rtindex = RTERangeTablePosn(pstate, rte, NULL);

	varattno = 1;
	ln = list_head(rte->eref->colnames);
	foreach(lt, rte->subquery->targetList)
	{
		TargetEntry *te = lfirst(lt);
		Var		   *varnode;
		char	   *resname;
		TargetEntry *tmp;

		if (te->resjunk)
			continue;

		Assert(varattno == te->resno);

		/* no transform here, just use `te->expr` */
		varnode = makeVar(rtindex, varattno,
						  exprType((Node *) te->expr),
						  exprTypmod((Node *) te->expr),
						  exprCollation((Node *) te->expr),
						  0);

		resname = strVal(lfirst(ln));

		tmp = makeTargetEntry((Expr *) varnode,
							  (AttrNumber) pstate->p_next_resno++,
							  resname,
							  false);
		targetlist = lappend(targetlist, tmp);

		varattno++;
		ln = lnext(ln);
	}

	return targetlist;
}

static List *
makeTargetListFromJoin(ParseState *pstate, RangeTblEntry *rte)
{
	List	   *targetlist = NIL;
	ListCell   *lt;
	ListCell   *ln;

	AssertArg(rte->rtekind == RTE_JOIN);

	forboth(lt, rte->joinaliasvars, ln, rte->eref->colnames)
	{
		Var		   *varnode = lfirst(lt);
		char	   *resname = strVal(lfirst(ln));
		TargetEntry *tmp;

		tmp = makeTargetEntry((Expr *) varnode,
							  (AttrNumber) pstate->p_next_resno++,
							  resname,
							  false);
		targetlist = lappend(targetlist, tmp);
	}

	return targetlist;
}

static TargetEntry *
makeWholeRowTarget(ParseState *pstate, RangeTblEntry *rte)
{
	int			rtindex;
	Var		   *varnode;

	rtindex = RTERangeTablePosn(pstate, rte, NULL);

	varnode = makeWholeRowVar(rte, rtindex, 0, false);
	varnode->location = -1;

	markVarForSelectPriv(pstate, varnode, rte);

	return makeTargetEntry((Expr *) varnode,
						   (AttrNumber) pstate->p_next_resno++,
						   rte->eref->aliasname,
						   false);
}

static TargetEntry *
findTarget(List *targetList, char *resname)
{
	ListCell *lt;
	TargetEntry *te = NULL;

	if (resname == NULL)
		return NULL;

	foreach(lt, targetList)
	{
		te = lfirst(lt);

		if (te->resjunk)
			continue;

		if (strcmp(te->resname, resname) == 0)
			return te;
	}

	return NULL;
}

static Node *
makeVertexExpr(ParseState *pstate, RangeTblEntry *rte, int location)
{
	Node	   *id;
	Node	   *prop_map;

	id = getColumnVar(pstate, rte, AG_ELEM_LOCAL_ID);
	prop_map = getColumnVar(pstate, rte, AG_ELEM_PROP_MAP);

	return makeTypedRowExpr(list_make2(id, prop_map), VERTEXOID, location);
}

static Node *
makeEdgeExpr(ParseState *pstate, RangeTblEntry *rte, int location)
{
	Node	   *id;
	Node	   *start;
	Node	   *end;
	Node	   *prop_map;

	id = getColumnVar(pstate, rte, AG_ELEM_LOCAL_ID);
	start = getColumnVar(pstate, rte, AG_START_ID);
	end = getColumnVar(pstate, rte, AG_END_ID);
	prop_map = getColumnVar(pstate, rte, AG_ELEM_PROP_MAP);

	return makeTypedRowExpr(list_make4(id, start, end, prop_map),
							EDGEOID, location);
}

static Node *
makePathVertexExpr(ParseState *pstate, Node *obj)
{
	if (IsA(obj, RangeTblEntry))
	{
		return makeVertexExpr(pstate, (RangeTblEntry *) obj, -1);
	}
	else
	{
		TargetEntry *te = (TargetEntry *) obj;

		AssertArg(IsA(obj, TargetEntry));
		AssertArg(exprType((Node *) te->expr) == VERTEXOID);

		return (Node *) te->expr;
	}
}

static Node *
makeGraphpath(List *vertices, List *edges, int location)
{
	Node	   *v_arr;
	Node	   *e_arr;

	v_arr = makeArrayExpr(VERTEXARRAYOID, VERTEXOID, vertices);
	e_arr = makeArrayExpr(EDGEARRAYOID, EDGEOID, edges);

	return makeTypedRowExpr(list_make2(v_arr, e_arr), GRAPHPATHOID, location);
}

static Node *
getColumnVar(ParseState *pstate, RangeTblEntry *rte, char *colname)
{
	ListCell   *lcn;
	int			attrno;
	Var		   *var;

	attrno = 1;
	foreach(lcn, rte->eref->colnames)
	{
		const char *tmp = strVal(lfirst(lcn));

		if (strcmp(tmp, colname) == 0)
		{
			/*
			 * NOTE: no ambiguous reference check here
			 *       since all column names in `rte` are unique
			 */

			var = make_var(pstate, rte, attrno, -1);

			/* require read access to the column */
			markVarForSelectPriv(pstate, var, rte);

			return (Node *) var;
		}

		attrno++;
	}

	elog(ERROR, "column \"%s\" not found (internal error)", colname);
	return NULL;
}

static Node *
getExprField(Expr *expr, char *fname)
{
	Oid			typoid;
	TupleDesc	tupdesc;
	int			idx;
	Form_pg_attribute attr = NULL;
	FieldSelect *fselect;

	typoid = exprType((Node *) expr);

	tupdesc = lookup_rowtype_tupdesc_copy(typoid, -1);
	for (idx = 0; idx < tupdesc->natts; idx++)
	{
		attr = tupdesc->attrs[idx];

		if (namestrcmp(&attr->attname, fname) == 0)
			break;
	}
	Assert(idx < tupdesc->natts);

	fselect = makeNode(FieldSelect);
	fselect->arg = expr;
	fselect->fieldnum = idx + 1;
	fselect->resulttype = attr->atttypid;
	fselect->resulttypmod = attr->atttypmod;
	fselect->resultcollid = attr->attcollation;

	return (Node *) fselect;
}

/* same as makeAlias() but no pstrdup(aliasname) */
static Alias *
makeAliasNoDup(char *aliasname, List *colnames)
{
	Alias *alias;

	alias = makeNode(Alias);
	alias->aliasname = aliasname;
	alias->colnames = colnames;

	return alias;
}

static Alias *
makeAliasOptUnique(char *aliasname)
{
	aliasname = (aliasname == NULL ? genUniqueName() : pstrdup(aliasname));
	return makeAliasNoDup(aliasname, NIL);
}

static Node *
makeArrayExpr(Oid typarray, Oid typoid, List *elems)
{
	ArrayExpr *arr = makeNode(ArrayExpr);

	arr->array_typeid = typarray;
	arr->element_typeid = typoid;
	arr->elements = elems;
	arr->multidims = false;
	arr->location = -1;

	return (Node *) arr;
}

static Node *
makeTypedRowExpr(List *args, Oid typoid, int location)
{
	RowExpr *row = makeNode(RowExpr);

	row->args = args;
	row->row_typeid = typoid;
	row->row_format = COERCE_EXPLICIT_CAST;
	row->location = location;

	return (Node *) row;
}

static Node *
qualAndExpr(Node *qual, Node *expr)
{
	if (qual == NULL)
		return expr;

	if (expr == NULL)
		return qual;

	if (IsA(qual, BoolExpr))
	{
		BoolExpr *bexpr = (BoolExpr *) qual;

		if (bexpr->boolop == AND_EXPR)
		{
			bexpr->args = lappend(bexpr->args, expr);
			return qual;
		}
	}

	return (Node *) makeBoolExpr(AND_EXPR, list_make2(qual, expr), -1);
}

static ResTarget *
makeSimpleResTarget(char *field, char *name)
{
	ColumnRef *cref;

	cref = makeNode(ColumnRef);
	cref->fields = list_make1(makeString(pstrdup(field)));
	cref->location = -1;

	return makeResTarget((Node *) cref, name);
}

static ResTarget *
makeFieldsResTarget(List *fields, char *name)
{
	ColumnRef *cref;

	cref = makeNode(ColumnRef);
	cref->fields = fields;
	cref->location = -1;

	return makeResTarget((Node *) cref, name);
}

static ResTarget *
makeResTarget(Node *val, char *name)
{
	ResTarget *res;

	res = makeNode(ResTarget);
	if (name != NULL)
		res->name = pstrdup(name);
	res->val = val;
	res->location = -1;

	return res;
}

static A_Const *
makeIntConst(int val)
{
	A_Const *c;

	c = makeNode(A_Const);
	c->val.type = T_Integer;
	c->val.val.ival = val;
	c->location = -1;

	return c;
}

/* generate unique name */
static char *
genUniqueName(void)
{
	/* NOTE: safe unless there are more than 2^32 anonymous names at once */
	static uint32 seq = 0;

	char data[NAMEDATALEN];

	snprintf(data, sizeof(data), "<%010u>", seq++);

	return pstrdup(data);
}
