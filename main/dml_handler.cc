#include <main/dml_handler.hh>
#include <main/rewrite_main.hh>
#include <parser/lex_util.hh>
#include <main/rewrite_util.hh>
#include <main/dispatcher.hh>

extern CItemTypesDir itemTypes;

// Prototypes.
static void
process_select_lex(LEX *lex, Analysis & a);

static void
process_select_lex(st_select_lex *select_lex, Analysis & a);

static void
process_filters_lex(st_select_lex * select_lex, Analysis & a);

static inline void
analyze_update(Item_field * field, Item * val, Analysis & a);

static st_select_lex *
rewrite_filters_lex(st_select_lex * select_lex, Analysis & a);

static bool
needsSalt(OLK olk);

static st_select_lex *
rewrite_select_lex(st_select_lex *select_lex, Analysis & a);

static void
process_table_list(List<TABLE_LIST> *tll, Analysis & a);

class InsertHandler : public DMLHandler {
    virtual void gather(LEX *lex, Analysis &a) const {
        process_select_lex(lex, a);
    }

    virtual LEX **rewrite(LEX *lex, Analysis &a, unsigned *out_lex_count) const {
        LEX * new_lex = copy(lex);

        const std::string &table =
                lex->select_lex.table_list.first->table_name;

        //rewrite table name
        new_lex->select_lex.table_list.first = rewrite_table_list(lex->select_lex.table_list.first, a);

        // fields
        std::vector<FieldMeta *> fmVec;
        if (lex->field_list.head()) {
            auto it = List_iterator<Item>(lex->field_list);
            List<Item> newList;
            for (;;) {
                Item *i = it++;
                if (!i)
                    break;
                assert(i->type() == Item::FIELD_ITEM);
                Item_field *ifd = static_cast<Item_field*>(i);
                //cerr << "field " << ifd->table_name << "." << ifd->field_name << endl;
                fmVec.push_back(a.getFieldMeta(ifd->table_name, ifd->field_name));
                std::vector<Item *> l;
                itemTypes.do_rewrite_insert(i, a, l, NULL);
                for (auto it0 = l.begin(); it0 != l.end(); ++it0) {
                    newList.push_back(*it0);
                }
            }
            new_lex->field_list = newList;
        }

        if (fmVec.empty()) {
            // Use the table order.
            TableMeta *tm = a.getTableMeta(table);
            std::vector<FieldMeta *> fmetas = tm->orderedFieldMetas();
            fmVec.assign(fmetas.begin(), fmetas.end());
        }

        // values
        if (lex->many_values.head()) {
            auto it = List_iterator<List_item>(lex->many_values);
            List<List_item> newList;
            for (;;) {
                List_item *li = it++;
                if (!li)
                    break;
                assert(li->elements == fmVec.size());
                List<Item> *newList0 = new List<Item>();
                auto it0 = List_iterator<Item>(*li);
                auto fmVecIt = fmVec.begin();
                for (;;) {
                    Item *i = it0++;
                    if (!i)
                        break;
                    std::vector<Item *> l;
                    // Prevent the dereferencing of a bad iterator if
                    // the user supplies more values than fields and the
                    // parser fails to throw an error.
                    // TODO(burrows): It seems like the expected behavior
                    // is for the parser to catch this bad state, so we
                    // will fail until further notice.
                    assert(fmVecIt != fmVec.end());
                    itemTypes.do_rewrite_insert(i, a, l, *fmVecIt);
                    for (auto it1 = l.begin(); it1 != l.end(); ++it1) {
                        newList0->push_back(*it1);
                        /*String s;
                        (*it1)->print(&s, QT_ORDINARY);
                        cerr << s << endl;*/
                    }
                    ++fmVecIt;
                }
                newList.push_back(newList0);
            }
            new_lex->many_values = newList;
        }

        return single_lex_output(new_lex, out_lex_count);
    }
};

class UpdateHandler : public DMLHandler {
    virtual void gather(LEX *lex, Analysis &a) const {
        process_table_list(&lex->select_lex.top_join_list, a);

        if (lex->select_lex.item_list.head()) {
            assert(lex->value_list.head());

            auto fd_it = List_iterator<Item>(lex->select_lex.item_list);
            auto val_it = List_iterator<Item>(lex->value_list);

            for (;;) {
                Item *i = fd_it++;
                Item * val = val_it++;
                if (!i)
                    break;
                assert(val != NULL);
                assert(i->type() == Item::FIELD_ITEM);
                Item_field *ifd = static_cast<Item_field*>(i);
                analyze_update(ifd, val, a);
            }
        }

        process_filters_lex(&lex->select_lex, a);
    }

    virtual LEX **rewrite(LEX *lex, Analysis &a, unsigned *out_lex_count) const {
        LEX * new_lex = copy(lex);

        LOG(cdb_v) << "rewriting update \n";

        assert_s(lex->select_lex.item_list.head(), "update needs to have item_list");

        // Rewrite table name
        new_lex->select_lex.top_join_list =
            rewrite_table_list(lex->select_lex.top_join_list, a);

        // Rewrite filters
        set_select_lex(new_lex, rewrite_filters_lex(&new_lex->select_lex, a));

        // Rewrite SET values
        bool invalids = false;

        assert(lex->select_lex.item_list.head());
        assert(lex->value_list.head());

        List<Item> res_items, res_vals;

        auto fd_it = List_iterator<Item>(lex->select_lex.item_list);
        auto val_it = List_iterator<Item>(lex->value_list);

        // Look through all pairs in set: fd = val
        for (;;) {
            Item * i = fd_it++;
            if (!i) {
                // Ensure that we were not dealing with an invalid query where
                // we had more values than fields.
                Item *v = val_it++;
                assert(NULL == v);
                break;
            }
            assert(i->type() == Item::FIELD_ITEM);
            Item_field * fd = static_cast<Item_field*>(i);

            FieldMeta * fm = a.getFieldMeta(fd->table_name, fd->field_name);

            Item * val = val_it++;
            assert(val != NULL);

            if (!fm->isEncrypted()) { // not encrypted field
                res_items.push_back(fd);
                res_vals.push_back(val);
                continue;
            }

            // Encrypted field

            RewritePlan * rp = getAssert(a.rewritePlans, val);
            EncSet r_es = rp->es_out.intersect(EncSet(fm));
            if (r_es.empty()) {
                /*
                 * FIXME(burrows): Change error message.
                cerr << "update cannot be performed BECAUSE " << i << " supports " << fm->encdesc << "\n BUT " \
                     << val << " can only provide " << rp->es_out << " BECAUSE " << rp->r << "\n";
                */
                assert(false);
            }

            // Determine salt for field
            bool add_salt = false;
            if (fm->has_salt) {
                auto it_salt = a.salts.find(fm);
                if ((it_salt == a.salts.end()) && needsSalt(r_es)) {
                    add_salt = true;
                    salt_type salt = randomValue();
                    a.salts[fm] = salt;
                }
            }

            Item * rew_fd = NULL;

            // Rewrite field-value pair for every onion possible
            for (auto pair : r_es.osl) {
                OLK olk = {pair.first, pair.second.first, fm};
                RewritePlan *rp_i = getAssert(a.rewritePlans, i);
                res_items.push_back(rew_fd = itemTypes.do_rewrite(i, olk, rp_i,
                                                                  a));
                RewritePlan *rp_val = getAssert(a.rewritePlans, val);
                res_vals.push_back(itemTypes.do_rewrite(val, olk, rp_val, a));
            }

            // Determine if the query invalidates onions.
            invalids = invalids || invalidates(fm, r_es);

            // Add the salt field
            if (add_salt) {
                salt_type salt = a.salts[fm];
                assert(rew_fd);
                res_items.push_back(make_item((Item_field *)rew_fd,
                                              fm->getSaltName()));
                res_vals.push_back(new Item_int((ulonglong) salt));
            }

        }

        //TODO: cleanup old item and value list

        new_lex->select_lex.item_list = res_items;
        new_lex->value_list = res_vals;

        if (false == invalids) {
            return single_lex_output(new_lex, out_lex_count);
        } else {
            return refresh_onions(lex, new_lex, a, out_lex_count);
        }
    }

private:
    LEX ** refresh_onions(LEX *lex, LEX *new_lex, Analysis &a,
                          unsigned *out_lex_count) const
    {
        // TODO(burrows): Should support multiple tables in a single UPDATE.
        const std::string plain_table =
            lex->select_lex.top_join_list.head()->table_name;
        std::string where_clause;
		if (lex->select_lex.where) {
			std::ostringstream where_stream;
			where_stream << " " << *lex->select_lex.where << " ";
			where_clause = where_stream.str();
		} else {	// HACK: Handle empty WHERE clause.
			where_clause = " TRUE ";
		}

        // Retrieve rows from database.
        std::ostringstream select_stream;
        select_stream << " SELECT * FROM " << plain_table
                      << " WHERE " << where_clause << ";";
        const ResType * const select_res_type =
            executeQuery(*a.rewriter, select_stream.str());
        assert(select_res_type);
        if (select_res_type->rows.size() == 0) { // No work to be done.
            return single_lex_output(new_lex, out_lex_count);
        }

		const auto itemJoin = [](std::vector<Item*> row) {
			return "(" + vector_join<Item*>(row, ",", ItemToString) + ")";
        };
        const std::string values_string =
            vector_join<std::vector<Item*>>(select_res_type->rows, ",",
                                            itemJoin);
        delete select_res_type;

        // Push the plaintext rows to the embedded database.
        std::ostringstream push_stream;
        push_stream << " INSERT INTO " << plain_table
                    << " VALUES " << values_string << ";";
        assert(a.ps->e_conn->execute(push_stream.str()));

        // Run the original (unmodified) query on the data in the embedded
        // database.
        std::ostringstream query_stream;
        query_stream << *lex;
        assert(a.ps->e_conn->execute(query_stream.str()));

        // > Collect the results from the embedded database.
        // > This code relies on single threaded access to the database
        //   and on the fact that the database is cleaned up after every such
        //   operation.
        DBResult *dbres;
        std::ostringstream select_results_stream;
        select_results_stream << " SELECT * FROM " << plain_table << ";";
        assert(a.ps->e_conn->execute(select_results_stream.str(), dbres));

        // FIXME(burrows): Use general join.
        ScopedMySQLRes r(dbres->n);
        MYSQL_ROW row;
        std::string output_rows = " ";
        const unsigned long field_count = r.res()->field_count;
        while ((row = mysql_fetch_row(r.res()))) {
            unsigned long *l = mysql_fetch_lengths(r.res());
            assert(l != NULL);

            output_rows.append(" ( ");
            for (unsigned long field_index = 0; field_index < field_count;
                 ++field_index) {
                std::string field_data;
                if (row[field_index]) {
                    field_data = std::string(row[field_index],
                                             l[field_index]);
                } else {    // Handle NULL values.
                    field_data = std::string("NULL");
                }
                output_rows.append(field_data);
                if (field_index + 1 < field_count) {
                    output_rows.append(", ");
                }
            }
            output_rows.append(" ) ,");
        }
        output_rows = output_rows.substr(0, output_rows.length() - 1);

        // Cleanup the embedded database.
        std::ostringstream cleanup_stream;
        cleanup_stream << "DELETE FROM " << plain_table << ";";
        assert(a.ps->e_conn->execute(cleanup_stream.str()));

        // > Add each row from the embedded database to the data database.
        std::ostringstream push_results_stream;
        push_results_stream << " INSERT INTO " << plain_table
                            << " VALUES " << output_rows << ";";
        Analysis insert_analysis = Analysis(a.ps);
        insert_analysis.rewriter = a.rewriter;
        // FIXME(burrows): Memleak.
        // Freeing the query_parse (or using an automatic variable and
        // letting it cleanup itself) will call the query_parse
        // destructor which calls THD::cleanup_after_query which
        // results in all of our Items_* being freed.
        // THD::cleanup_after_query
        //     Query_arena::free_items
        //         Item::delete_self).
        query_parse * const parse =
			new query_parse(a.ps->conn->getCurDBName(),
                            push_results_stream.str());
        const SQLHandler *handler =
            a.rewriter->dml_dispatcher->dispatch(parse->lex());
        unsigned final_insert_out_lex_count;
        LEX ** const final_insert_lex_arr =
            handler->transformLex(parse->lex(), insert_analysis,
                                  push_results_stream.str(),
                                  &final_insert_out_lex_count);
        assert(final_insert_lex_arr && 1 == final_insert_out_lex_count);
        LEX * const final_insert_lex = final_insert_lex_arr[0];

        // DELETE the rows matching the WHERE clause from the database.
        std::ostringstream delete_stream;
        delete_stream << " DELETE FROM " << plain_table
                      << " WHERE " << where_clause << ";";
        Analysis delete_analysis = Analysis(a.ps);
        delete_analysis.rewriter = a.rewriter;
        // FIXME(burrows): Identical memleak.
        query_parse * const delete_parse =
            new query_parse(a.ps->conn->getCurDBName(),
                            delete_stream.str());
        const SQLHandler * const delete_handler =
            a.rewriter->dml_dispatcher->dispatch(delete_parse->lex());
        unsigned delete_out_lex_count;
        LEX ** const delete_lex_arr =
            delete_handler->transformLex(delete_parse->lex(),
                                         delete_analysis,
                                         delete_stream.str(),
                                         &delete_out_lex_count);
        assert(delete_lex_arr && 1 == delete_out_lex_count);
        LEX * const delete_lex = delete_lex_arr[0];

        // FIXME(burrows): Order matters, how to enforce?
        LEX **out_lex = new LEX*[4];
        out_lex[0] = begin_transaction_lex(a);
        out_lex[1] = delete_lex;
        out_lex[2] = final_insert_lex;
        out_lex[3] = commit_transaction_lex(a);
        *out_lex_count = 4;
        return out_lex;
    }

    bool invalidates(FieldMeta * fm, const EncSet & es) const {
        for (auto o_l : fm->children) {
            onion o = o_l.first->getValue();
            if (es.osl.find(o) == es.osl.end()) {
                return true;
            }
        }

        return false;
    }
};

class DeleteHandler : public DMLHandler {
    virtual void gather(LEX *lex, Analysis &a) const {
        process_select_lex(lex, a);
    }

    virtual LEX **rewrite(LEX *lex, Analysis &a, unsigned *out_lex_count) const {
        LEX * new_lex = copy(lex);
        new_lex->query_tables = rewrite_table_list(lex->query_tables, a);
        set_select_lex(new_lex, rewrite_select_lex(&new_lex->select_lex, a));

        return single_lex_output(new_lex, out_lex_count);
    }
};

class SelectHandler : public DMLHandler {
    virtual void gather(LEX *lex, Analysis &a) const {
        process_select_lex(lex, a);
    }

    virtual LEX **rewrite(LEX *lex, Analysis &a, unsigned *out_lex_count) const {
        LEX * new_lex = copy(lex);
        new_lex->select_lex.top_join_list = rewrite_table_list(lex->select_lex.top_join_list, a);
        set_select_lex(new_lex, rewrite_select_lex(&new_lex->select_lex, a));

        return single_lex_output(new_lex, out_lex_count);
    }
};

LEX **DMLHandler::transformLex(LEX *lex, Analysis &analysis,
                               const std::string &q,
                               unsigned *out_lex_count) const
{
    this->gather(lex, analysis);

    return this->rewrite(lex, analysis, out_lex_count);
}

// Helpers.

static void
process_order(Analysis & a, SQL_I_List<ORDER> & lst) {

    for (ORDER *o = lst.first; o; o = o->next) {
	reason r;
	gather(*o->item, r, a);
    }

}

//TODO: not clear how these process_*_lex and rewrite_*_lex overlap, cleanup
static void
process_filters_lex(st_select_lex * select_lex, Analysis & a) {

    if (select_lex->where) {
	analyze(select_lex->where, a);
    }

    /*if (select_lex->join &&
        select_lex->join->conds &&
        select_lex->where != select_lex->join->conds)
        analyze(select_lex->join->conds, reason(FULL_EncSet, "join->conds", select_lex->join->conds, 0), a);*/

    if (select_lex->having)
        analyze(select_lex->having, a);

    process_order(a, select_lex->group_list);
    process_order(a, select_lex->order_list);

}

static void
process_select_lex(LEX *lex, Analysis & a)
{
    process_table_list(&lex->select_lex.top_join_list, a);
    process_select_lex(&lex->select_lex, a);
}

static void
process_select_lex(st_select_lex *select_lex, Analysis & a)
{
    //select clause
    auto item_it = List_iterator<Item>(select_lex->item_list);
    for (;;) {
        Item *item = item_it++;
        if (!item)
            break;

        analyze(item, a);
    }

    process_filters_lex(select_lex, a);
}

// TODO:
// should be able to support general updates such as
// UPDATE table SET x = 2, y = y + 1, z = y+2, z =z +1, w = y, l = x
// this has a few corner cases, since y can only use HOM
// onion so does w, but not l

//analyzes an expression of the form field = val expression from
// an UPDATE
static inline void
analyze_update(Item_field * field, Item * val, Analysis & a) {

    reason r;
    a.rewritePlans[val] = gather(val, r, a);
    a.rewritePlans[field] = gather(field, r, a);

    //TODO: an optimization could be performed here to support more updates
    // For example: SET x = x+1, x = 2 --> no need to invalidate DET and OPE
    // onions because first SET does not matter
}

static void
rewrite_order(Analysis & a, SQL_I_List<ORDER> & lst,
	      const EncSet & constr, const std::string & name) {
    ORDER * prev = NULL;
    for (ORDER *o = lst.first; o; o = o->next) {
	Item * i = *o->item;
	RewritePlan * rp = getAssert(a.rewritePlans, i);
	assert(rp);
	EncSet es = constr.intersect(rp->es_out);
	if (es.empty()) {
            std::cerr << " cannot support query because " << name
                      << " item " << i << " needs to output any of "
                      << constr << "\n"
		      << " BUT it can only output " << rp->es_out
                      << " BECAUSE " << "(" << rp->r << ")\n";
	    assert(false);
	}
	OLK olk = es.chooseOne();

	Item * new_item = itemTypes.do_rewrite(*o->item, olk, rp, a);
	ORDER * neworder = make_order(o, new_item);
	if (prev == NULL) {
	    lst = *oneElemList(neworder);
	} else {
	    prev->next = neworder;
	}
	prev = neworder;
    }
}

static st_select_lex *
rewrite_filters_lex(st_select_lex * select_lex, Analysis & a) {

    st_select_lex * new_select_lex = copy(select_lex);

    if (select_lex->where) {
        set_where(new_select_lex, rewrite(select_lex->where, PLAIN_OLK, a));
    }
    //  if (select_lex->join &&
	//     select_lex->join->conds &&
    //    select_lex->where != select_lex->join->conds) {
	//cerr << "select_lex join conds " << select_lex->join->conds << "\n";
	//rewrite(&select_lex->join->conds, a);
    //}

    if (select_lex->having)
        new_select_lex->having = rewrite(select_lex->having, PLAIN_OLK, a);

    rewrite_order(a, new_select_lex->group_list, EQ_EncSet, "group by");
    rewrite_order(a, new_select_lex->order_list, ORD_EncSet, "order by");

    return new_select_lex;
}

static void
addToReturn(ReturnMeta * rm, int pos, const OLK & constr,  bool has_salt) {
    ReturnField rf = ReturnField();
    rf.is_salt = false;
    rf.olk = constr;
    if (has_salt) {
        rf.pos_salt = pos+1;
    } else {
        rf.pos_salt = -1;
    }
    rm->rfmeta[pos] = rf;
}

static void
addToReturn(ReturnMeta * rm, int pos, const OLK & constr, bool has_salt,
            std::string name) {
    addToReturn(rm, pos, constr, has_salt);
    rm->rfmeta[pos].field_called = name;
}

static void
addSaltToReturn(ReturnMeta * rm, int pos) {
    ReturnField rf = ReturnField();
    rf.is_salt = true;
    rf.olk = OLK();
    rf.pos_salt = -1;
    rm->rfmeta[pos] = rf;
}

static void
rewrite_proj(Item * i, const RewritePlan * rp, Analysis & a, List<Item> & newList)
{
    OLK olk = rp->es_out.chooseOne();
    Item *ir = rewrite(i, olk, a);
    newList.push_back(ir);
    bool use_salt = needsSalt(olk);

    addToReturn(a.rmeta, a.pos++, olk, use_salt, i->name);

    if (use_salt) {
        newList.push_back(make_item((Item_field*) ir,
                                    olk.key->getSaltName()));
        addSaltToReturn(a.rmeta, a.pos++);
    }
}

static st_select_lex *
rewrite_select_lex(st_select_lex *select_lex, Analysis & a)
{
    st_select_lex * new_select_lex = copy(select_lex);

    LOG(cdb_v) << "rewrite select lex input is " << *select_lex;
    auto item_it = List_iterator<Item>(select_lex->item_list);

    List<Item> newList;
    for (;;) {
        Item *item = item_it++;
        if (!item)
            break;
        LOG(cdb_v) << "rewrite_select_lex " << *item << " with name " << item->name;
	rewrite_proj(item, getAssert(a.rewritePlans, item), a, newList);
    }

    // TODO(stephentu): investigate whether or not this is a memory leak
    new_select_lex->item_list = newList;

    return rewrite_filters_lex(new_select_lex, a);
}

static bool
needsSalt(OLK olk) {
    return olk.key && olk.key->has_salt && needsSalt(olk.l);
}

static void
process_table_list(List<TABLE_LIST> *tll, Analysis & a)
{
    /*
     * later, need to rewrite different joins, e.g.
     * SELECT g2_ChildEntity.g_id, IF(ai0.g_id IS NULL, 1, 0) AS albumsFirst, g2_Item.g_originationTimestamp FROM g2_ChildEntity LEFT JOIN g2_AlbumItem AS ai0 ON g2_ChildEntity.g_id = ai0.g_id INNER JOIN g2_Item ON g2_ChildEntity.g_id = g2_Item.g_id INNER JOIN g2_AccessSubscriberMap ON g2_ChildEntity.g_id = g2_AccessSubscriberMap.g_itemId ...
     */

    List_iterator<TABLE_LIST> join_it(*tll);
    for (;;) {
        TABLE_LIST *t = join_it++;
        if (!t)
            break;

        if (t->nested_join) {
            process_table_list(&t->nested_join->join_list, a);
            return;
        }

        if (t->on_expr)
            analyze(t->on_expr, a);

        //std::string db(t->db, t->db_length);
        //std::string table_name(t->table_name, t->table_name_length);
        //std::string alias(t->alias);

        if (t->is_alias)
            assert(a.addAlias(t->alias, t->table_name));

        // Handles SUBSELECTs in table clause.
        if (t->derived) {
            st_select_lex_unit *u = t->derived;
             // Not quite right, in terms of softness:
             // should really come from the items that eventually
             // reference columns in this derived table.
            process_select_lex(u->first_select(), a);
        }
    }
}

// FIXME: Add test to make sure handlers added successfully.
SQLDispatcher *buildDMLDispatcher()
{
    DMLHandler *h;
    SQLDispatcher *dispatcher = new SQLDispatcher();
    
    h = new InsertHandler();
    dispatcher->addHandler(SQLCOM_INSERT, h);
    dispatcher->addHandler(SQLCOM_REPLACE, h);

    h = new UpdateHandler;
    dispatcher->addHandler(SQLCOM_UPDATE, h);

    h = new DeleteHandler;
    dispatcher->addHandler(SQLCOM_DELETE, h);

    h = new SelectHandler;
    dispatcher->addHandler(SQLCOM_SELECT, h);

    return dispatcher;
}
