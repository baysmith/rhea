//---------------------------------------------------------------------------
// solver.cpp
//
// This file is part of Rhea.  Rhea is free software: you can redistribute
// it and/or modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// Rhea is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
// more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with Rhea.  If not, see <http://www.gnu.org/licenses/>.
//
// Copyright 2012, nocte@hippie.nu
//---------------------------------------------------------------------------
#include "simplex_solver.hpp"

#include <algorithm>
#include <queue>

#include "errors.hpp"
#include "slack_variable.hpp"
#include "dummy_variable.hpp"

namespace rhea {

template <class container, typename func>
void remove_from_container_if(container& c, func pred)
{
    c.erase(std::remove_if(c.begin(), c.end(), pred));
}

simplex_solver::expression_result
simplex_solver::make_expression(const constraint_ref& c)
{
    expression_result result;

    auto& expr (result.expr);
    auto cexpr (c->expression());
    expr.set_constant(cexpr.constant());

    for (const auto& term : cexpr.terms())
    {
        if (is_basic_var(term.first))
            expr += row_expression(term.first) * term.second;
        else
            expr += term;
    }

    if (c->is_inequality())
    {
        // cn is an inequality, so Add a slack variable.  The original
        // constraint is expr>=0, so that the resulting equality is
        // expr-slackVar=0.  If cn is also non-required Add a negative
        // error variable, giving
        //    expr-slackVar = -errorVar, in other words
        //    expr-slackVar+errorVar=0.
        // Since both of these variables are newly created we can just Add
        // them to the Expression (they can't be basic).
        variable slack (std::make_shared<slack_variable>("s"));
        expr.set(slack, -1);
        marker_vars_[c] = slack;
        constraints_marked_[slack] = c;

        if (!c->is_required())
        {
            variable eminus (std::make_shared<slack_variable>("em"));
            expr.set(eminus, 1);
            linear_expression&  row (row_expression(objective_));
            double              sw  (c->adjusted_symbolic_weight());
            row += linear_expression::term(eminus, sw);
            error_vars_[c].insert(eminus);
            note_added_variable(eminus, objective_);
        }
    }
    else
    {
        // c is an equality
        if (c->is_required())
        {
            // Add a dummy variable to the Expression to serve as a marker
            // for this constraint.  The dummy variable is never allowed to
            // enter the basis when pivoting.
            variable dum (std::make_shared<dummy_variable>());
            expr.set(dum, 1);
            marker_vars_[c] = dum;
            constraints_marked_[dum] = c;
        }
        else
        {
            // cn is a non-required equality.  Add a positive and a negative
            // error variable, making the resulting constraint
            //       expr = eplus - eminus,
            // in other words:  expr-eplus+eminus=0
            variable eplus  (std::make_shared<slack_variable>("ep"));
            variable eminus (std::make_shared<slack_variable>("em"));

            expr.set(eplus, -1);
            expr.set(eminus, 1);

            marker_vars_[c] = eplus;
            constraints_marked_[eplus] = c;

            auto& rowexp (row_expression(objective_));
            double coeff (c->adjusted_symbolic_weight());

            rowexp.set(eplus, coeff);
            note_added_variable(eplus, objective_);
            error_vars_[c].insert(eplus);

            rowexp.set(eminus, coeff);
            note_added_variable(eminus, objective_);
            error_vars_[c].insert(eminus);

            if (c->is_stay_constraint())
            {
                stay_plus_error_vars_.push_back(eplus);
                stay_minus_error_vars_.push_back(eminus);
            }
            else if (c->is_edit_constraint())
            {
                result.plus  = eplus;
                result.minus = eminus;
                result.previous_constant = expr.constant();
            }
        }
    }

    // the Constant in the Expression should be non-negative.
    // If necessary normalize the Expression by multiplying by -1
    if (expr.constant() < 0)
        expr *= -1;

    return result;
}

simplex_solver&
simplex_solver::add_constraint(const constraint_ref& c)
{
    if (c->is_strict_inequality())
        throw strict_inequality_not_allowed();

    if (!c->read_only_variables().empty())
        throw readonly_not_allowed();

    auto ptr (dynamic_cast<edit_constraint*>(c.get()));
    if (ptr != nullptr)
    {
        assert(c->is_edit_constraint());
        edit_constraint& ec (*ptr);
        const auto& v (ec.var());
        if (!v.is_external() || (!is_basic_var(v) && !columns_has_key(v)))
            throw edit_misuse("edit constraints on variable not in tableau");

        auto i (std::find(edit_info_list_.begin(), edit_info_list_.end(), v));
        if (i != edit_info_list_.end())
        {
            edit_info_list_.emplace_back(v, nullptr, variable::nil(), variable::nil(), 0);
            return *this;
        }
    }

    auto r (make_expression(c));

    bool added_ok_directly (false);
    try
    {
        added_ok_directly = try_adding_directly(r.expr);
    }
    catch(required_failure& e)
    {
        remove_constraint_internal(c);
        throw;
    }

    if (!added_ok_directly)
    {
        if (!add_with_artificial_variable(r.expr))
        {
            remove_constraint_internal(c);
            throw required_failure();
        }
    }

    needs_solving_ = true;

    if (ptr != nullptr)
    {
        edit_constraint& ec (*ptr);
        edit_info_list_.emplace_back(ec.var(), c, r.plus, r.minus, r.previous_constant);
    }

    if (auto_solve_)
    {
        optimize(objective_);
        set_external_variables();
    }

    c->added_to(*this);

    return *this;
}

simplex_solver&
simplex_solver::add_point_stays(const std::vector<point>& points,
                                const strength& s, double weight)
{
    auto i (points.begin());
    for (const auto& pt : points)
    {
        add_point_stay(pt, s, weight);
        weight *= 2.0;
    }
    return *this;
}

simplex_solver&
simplex_solver::add_point_stay(const point& pt, const strength& s, double weight)
{
    add_stay(pt.x, s, weight);
    add_stay(pt.y, s, weight);
    return *this;
}

simplex_solver&
simplex_solver::remove_constraint_internal(const constraint_ref& c)
{
    needs_solving_ = true;
    reset_stay_constants();

    auto& rowexpr (row_expression(objective_));
    auto i (error_vars_.find(c));
    if (i != error_vars_.end())
    {
        for (const variable& var : i->second)
        {
            if (is_basic_var(var))
            {
                const linear_expression& expr (row_expression(var));
                rowexpr.add(expr * -c->adjusted_symbolic_weight(),
                            objective_, *this);
            }
            else
            {
                rowexpr.add(var, -c->adjusted_symbolic_weight(),
                            objective_, *this);
            }
        }
    }

    auto im (marker_vars_.find(c));
    if (im == marker_vars_.end())
        throw constraint_not_found();

    variable marker (im->second);
    marker_vars_.erase(im);
    constraints_marked_.erase(marker);

    if (!is_basic_var(marker))
    {
        // Try to make this marker variable basic.
        auto& col (columns_[marker]);
        bool exit_var_set (false);
        double min_ratio (0.0);
        variable exit_var (variable::nil());

        for (auto& v : col)
        {
            if (v.is_restricted())
            {
                auto& expr (row_expression(v));
                double coeff (expr.coefficient(marker));

                if (coeff >= 0.0)
                    continue; // Only consider negative coefficients

                double r (-expr.constant() / coeff);
                if (!exit_var_set || r < min_ratio)
                {
                    min_ratio = r;
                    exit_var = v;
                    exit_var_set = true;
                }
            }
        }
        // If we didn't set exitvar above, then either the marker
        // variable has a positive coefficient in all equations, or it
        // only occurs in equations for unrestricted variables.  If it
        // does occur in an equation for a restricted variable, pick the
        // equation that gives the smallest ratio.  (The row with the
        // marker variable will become infeasible, but all the other rows
        // will still be feasible; and we will be dropping the row with
        // the marker variable.  In effect we are removing the
        // non-negativity restriction on the marker variable.)
        if (!exit_var_set)
        {
            for (auto& v : col)
            {
                if (v.is_restricted())
                {
                    auto& expr (row_expression(v));
                    double coeff (expr.coefficient(marker));
                    double r (expr.constant() / coeff);
                    if (!exit_var_set || r < min_ratio)
                    {
                        min_ratio = r;
                        exit_var = v;
                        exit_var_set = true;
                    }
                }
            }
        }

        if (!exit_var_set)
        {
            // exitVar is still nil
            // If col is empty, then exitVar doesn't occur in any equations,
            // so just remove it.  Otherwise pick an exit var from among the
            // unrestricted variables whose equation involves the marker var
            if (col.empty())
            {
                remove_column(marker);
            }
            else
            {
                for (auto& v : col)
                {
                    if (v != objective_)
                    {
                        exit_var = v;
                        exit_var_set = true;
                        break;
                    }
                }
            }
            assert(exit_var_set);
        }

        if (exit_var_set)
            pivot(marker, exit_var);

    }

    if (is_basic_var(marker))
        remove_row(marker);

    // Delete any error variables.  If cn is an inequality, it also
    // contains a slack variable; but we use that as the marker variable
    // and so it has been deleted when we removed its row.
    if (i != error_vars_.end())
    {
        for (const auto& v : i->second)
        {
            if (v != marker)
                remove_column(v);
        }
    }

    if (c->is_stay_constraint())
    {
        if (i != error_vars_.end())
        {
            auto pred ([&](variable x){ return i->second.count(x) > 0; });
            remove_from_container_if(stay_plus_error_vars_, pred);
            remove_from_container_if(stay_minus_error_vars_, pred);
        }
    }
    else if (c->is_edit_constraint())
    {
        auto& ec (*dynamic_cast<edit_constraint*>(c.get()));
        auto ei (std::find(edit_info_list_.begin(), edit_info_list_.end(), ec.var()));
        assert(ei != edit_info_list_.end());
        remove_column(ei->minus); // plus is a marker var and gets removed later
        edit_info_list_.erase(ei);
    }

    if (i != error_vars_.end())
        error_vars_.erase(i);

    if (auto_solve_)
    {
        optimize(objective_);
        set_external_variables();
    }

    return *this;
}

void
simplex_solver::resolve()
{
    dual_optimize();
    set_external_variables();
    infeasible_rows_.clear();
    if (auto_reset_stay_constants_)
        reset_stay_constants();
}

simplex_solver&
simplex_solver::suggest_value(const variable& v, double x)
{
    auto ei (std::find(edit_info_list_.begin(), edit_info_list_.end(), v));
    if (ei == edit_info_list_.end())
        throw edit_misuse(v.name());

    double delta (x - ei->prev_constant);
    ei->prev_constant = x;
    delta_edit_constant(delta, ei->plus, ei->minus);

    return *this;
}

// Add the constraint expr=0 to the inequality tableau using an
// artificial variable.  To do this, create an artificial variable
// av and Add av=expr to the inequality tableau, then make av be 0.
// (Raise an exception if we can't attain av=0 -- and prepare explanation)
bool
simplex_solver::add_with_artificial_variable(linear_expression& expr)
{
    // The artificial objective is av, which we know is equal to expr
    // (which contains only parametric variables).
    variable av (std::make_shared<slack_variable>("av"));
    variable az (std::make_shared<objective_variable>("az"));
    linear_expression row (expr);

    // Objective is treated as a row in the tableau,
    // so do the substitution for its value (we are minimizing
    // the artificial variable).
    // This row will be removed from the tableau after optimizing.
    add_row(az, row);

    // Add the normal row to the tableau -- when artifical
    // variable is minimized to 0 (if possible)
    // this row remains in the tableau to maintain the constraint
    // we are trying to add.
    add_row(av, expr);

    // Try to optimize az to 0.
    // Note we are *not* optimizing the real objective, but optimizing
    // the artificial objective to see if the error in the constraint
    // we are adding can be set to 0.
    optimize(az);

    // Careful, we want to get the Expression that is in
    // the tableau, not the one we initialized it with!
    auto& tableau_row (row_expression(az));

    // Check that we were able to make the objective value 0
    // If not, the original constraint was not satisfiable
    if (!near_zero(tableau_row.constant()))
    {
        ///\todo build explanation
        return false;
    }

    if (is_basic_var(av))
    {
        const auto& e (row_expression(av));

        // Find another variable in this row and Pivot, so that av becomes parametric
        // If there isn't another variable in the row then
        // the tableau contains the equation av = 0  -- just delete av's row
        if (e.is_constant())
        {
            assert(near_zero(e.constant()));
            remove_row(av);
            return true;
        }
        variable entry (e.find_pivotable_variable());
        if (entry.is_nil())
        {
            ///\todo build explanation
            return false;
        }
        pivot(entry, av);
    }

    assert(is_parametric_var(av));
    remove_column(av);
    remove_row(az);

    return true;
}

simplex_solver&
simplex_solver::remove_edit_vars_to(size_t n)
{
    std::queue<variable> qv;
    variable_set still_editing; // Variables that we need to *not* remove
    size_t i (0);
    for (auto it (edit_info_list_.begin());
         it != edit_info_list_.end() && edit_info_list_.size() != n;
         ++it, ++i)
    {
        if (i >= n)
            qv.push(it->v);
        else
            still_editing.insert(it->v);

        while (!qv.empty())
        {
            // only remove the variable if it's not in the set of variable
            // from a previous nested outer edit
            // e.g., if I do:
            // Edit x,y
            // Edit w,h,x,y
            // EndEdit
            // The end edit needs to only get rid of the edits on w,h
            // not the ones on x,y
            auto& f (qv.front());
            if (still_editing.count(f) == 0)
                remove_edit_var(f);

            qv.pop();
        }

        while (edit_info_list_.size() > n)
            edit_info_list_.pop_back();
    }

    return *this;
}

bool
simplex_solver::try_adding_directly(linear_expression& expr)
{
    variable subj (choose_subject(expr));
    if (subj.is_nil())
        return false;

    expr.new_subject(subj);
    if (columns_has_key(subj))
        substitute_out(subj, expr);

    add_row(subj, expr);
    return true;
}

simplex_solver&
simplex_solver::remove_edit_var(const variable& v)
{
    auto i (std::find(edit_info_list_.begin(), edit_info_list_.end(), v));
    if (i == edit_info_list_.end())
        throw edit_misuse();

    remove_constraint(i->c);

    return *this;
}

// We are trying to Add the constraint expr=0 to the tableaux.  Try
// to choose a subject (a variable to become basic) from among the
// current variables in expr.  If expr contains any unrestricted
// variables, then we must choose an unrestricted variable as the
// subject.  Also, if the subject is new to the solver we won't have
// to do any substitutions, so we prefer new variables to ones that
// are currently noted as parametric.  If expr contains only
// restricted variables, if there is a restricted variable with a
// negative coefficient that is new to the solver we can make that
// the subject.  Otherwise we can't find a subject, so return nil.
// (In this last case we have to Add an artificial variable and use
// that variable as the subject -- this is done outside this method
// though.)
//
// Note: in checking for variables that are new to the solver, we
// ignore whether a variable occurs in the objective function, since
// new slack variables are added to the objective function by
// 'NewExpression:', which is called before this method.
variable
simplex_solver::choose_subject(linear_expression& expr)
{
    variable subj (variable::nil());
    bool found_unrestricted (false), found_new_restricted (false);

    for (auto& term : expr.terms())
    {
        const variable& v (term.first);
        double          c (term.second);

        if (found_unrestricted)
        {
            // We have already found an unrestricted variable.  The only
            // time we will want to use v instead of the current choice
            // 'subject' is if v is unrestricted and new to the solver and
            // 'subject' isn't new.  If this is the case just pick v
            // immediately and return.
            if (!v.is_restricted() && !columns_has_key(v))
                return v;
        }
        else
        {
            if (v.is_restricted())
            {
                // v is restricted.  If we have already found a suitable
                // restricted variable just stick with that.  Otherwise, if v
                // is new to the solver and has a negative coefficient pick
                // it.  Regarding being new to the solver -- if the variable
                // occurs only in the objective function we regard it as being
                // new to the solver, since error variables are added to the
                // objective function when we make the Expression.  We also
                // never pick a dummy variable here.
                if (!found_new_restricted && !v.is_dummy() && c < 0.0)
                {
                    auto i (columns_.find(v));
                    if (   i == columns_.end()
                        || (columns_.size() == 1 && columns_has_key(objective_)))
                    {
                        subj = v;
                        found_new_restricted = true;
                    }
                }
            }
            else
            {
                // v is unrestricted.
                // If v is also new to the solver just pick it now.
                subj = v;
                found_unrestricted = true;
            }
        }
    }

    if (!subj.is_nil())
        return subj;

    // Make one last check -- if all of the variables in expr are dummy
    // variables, then we can pick a dummy variable as the subject.
    double coeff (0);
    for (auto& term : expr.terms())
    {
        const variable& v (term.first);
        if (!v.is_dummy())
            return variable::nil(); // Nope, no luck.

        if (!columns_has_key(v))
        {
            subj = v;
            coeff = term.second;
        }
    }

    // If we get this far, all of the variables in the Expression should
    // be dummy variables.  If the Constant is nonzero we are trying to
    // Add an unsatisfiable required constraint.  (Remember that dummy
    // variables must take on a value of 0.)  Otherwise, if the Constant
    // is Zero, multiply by -1 if necessary to make the coefficient for
    // the subject negative."
    if (near_zero(expr.constant()))
        throw required_failure();

    if (coeff > 0)
        expr *= -1;

    return subj;
}

// Minimize the value of the objective.  (The tableau should already
// be feasible.)
void simplex_solver::optimize(const variable& v)
{
    auto& row (row_expression(v));

    variable entry (variable::nil()), exit (variable::nil());

    while (true)
    {
        double coeff (0);
        // Find the most negative coefficient in the objective function
        // (ignoring the non-pivotable dummy variables).  If all
        // coefficients are positive we're done.
        for (auto& p : row.terms())
        {
            const variable& v (p.first);
            double          c (p.second);

            if (v.is_pivotable() && c < coeff)
            {
                coeff = c;
                entry = v;
                break;
            }
        }

        // If all coefficients were positive (or if the objective
        // function has no pivotable variables) we are at an optimum.
        if (coeff >= -epsilon_)
            return;

        // Choose which variable to move out of the basis.
        // Only consider pivotable basic variables
        // (i.e. restricted, non-dummy variables).
        double min_ratio (std::numeric_limits<double>::max());
        double r (0);
        for (const variable& v : columns_[entry])
        {
            if (v.is_pivotable())
            {
                const auto& expr (row_expression(v));
                double coeff (expr.coefficient(entry));

                if (coeff >= 0) // Only consider negative coefficients
                    continue;

                r = -expr.constant() / coeff;
                if (r < min_ratio)
                {
                    min_ratio = r;
                    exit = v;
                }
            }
        }

        // If minRatio is still nil at this point, it means that the
        // objective function is unbounded, i.e. it can become
        // arbitrarily negative.  This should never happen in this
        // application.
        if (min_ratio == std::numeric_limits<double>::max())
            throw internal_error("objective function is unbounded");

        pivot(entry, exit);
    }
}

// Each of the non-required edits will be represented by an equation
// of the form
//    v = c + eplus - eminus
// where v is the variable with the edit, c is the previous edit
// value, and eplus and eminus are slack variables that hold the
// error in satisfying the edit constraint.  We are about to change
// something, and we want to fix the constants in the equations
// representing the edit constraints.  If one of eplus and eminus is
// basic, the other must occur only in the Expression for that basic
// error variable.  (They can't both be basic.)  Fix the Constant in
// this Expression.  Otherwise they are both nonbasic.  Find all of
// the expressions in which they occur, and fix the constants in
// those.  See the UIST paper for details.
// (This comment was for resetEditConstants(), but that is now
// gone since it was part of the screwey vector-based interface
// to resolveing. --02/15/99 gjb)
void
simplex_solver::delta_edit_constant(double delta, const variable& plus,
                                    const variable& minus)
{
    // Check if the variables are basic
    if (is_basic_var(plus))
    {
        auto& expr (row_expression(plus));
        expr.increment_constant(delta);
        if (expr.constant() < 0)
            infeasible_rows_.insert(plus);

        return;
    }
    if (is_basic_var(minus))
    {
        auto& expr (row_expression(minus));
        expr.increment_constant(-delta);
        if (expr.constant() < 0)
            infeasible_rows_.insert(minus);

        return;
    }

    // Neither is basic.  So they must both be nonbasic, and will both
    // occur in exactly the same expressions.  Find all the expressions
    // in which they occur by finding the column for the minusErrorVar
    // (it doesn't matter whether we look for that one or for
    // plusErrorVar).  Fix the constants in these expressions.
    for (auto& v : columns_[minus])
    {
        auto& expr (row_expression(v));
        expr.increment_constant(expr.coefficient(minus) * delta);

        if (v.is_restricted() && expr.constant() < 0)
            infeasible_rows_.insert(v);
    }
}

void
simplex_solver::dual_optimize()
{
    auto& row (row_expression(objective_));
    while (!infeasible_rows_.empty())
    {
        auto ii (infeasible_rows_.begin());
        variable exit_var (*ii);
        infeasible_rows_.erase(ii);
        variable entry_var;

        // exit_var might have become basic after some other pivoting
        // so allow for the case of its not being there any longer.
        if (!is_basic_var(exit_var))
            continue;

        auto& expr (row_expression(exit_var));
        if (expr.constant() >= 0)
            continue; // Skip this row if it's feasible.

        double ratio (std::numeric_limits<double>::max());
        double r (0);

        for (auto& p : expr.terms())
        {
            const variable& v (p.first);
            double          c (p.second);
            if (c > 0 && v.is_pivotable())
            {
                r = row.coefficient(v) / c;
                if (r < ratio)
                {
                    entry_var = v;
                    ratio = r;
                }
            }
        }

        if (ratio == std::numeric_limits<double>::max())
            throw internal_error("ratio == dbl_max");

        pivot(entry_var, exit_var);
    }
}

// Do a Pivot.  Move entryVar into the basis (i.e. make it a basic variable),
// and move exitVar out of the basis (i.e., make it a parametric variable)
void
simplex_solver::pivot(const variable& entry, const variable& exit)
{
    // The entryVar might be non-pivotable if we're doing a RemoveConstraint --
    // otherwise it should be a pivotable variable -- enforced at call sites,
    // hopefully

    // expr is the Expression for the exit variable (about to leave the basis) --
    // so that the old tableau includes the equation:
    //   exitVar = expr
    auto expr (remove_row(exit));

    // Compute an Expression for the entry variable.  Since expr has
    // been deleted from the tableau we can destructively modify it to
    // build this Expression.
    expr.change_subject(exit, entry);
    substitute_out(entry, expr);

    if (entry.is_external())
        external_parametric_vars_.erase(entry);

    add_row(entry, expr);
}

// Each of the non-required stays will be represented by an equation
// of the form
//     v = c + eplus - eminus
// where v is the variable with the stay, c is the previous value of
// v, and eplus and eminus are slack variables that hold the error
// in satisfying the stay constraint.  We are about to change
// something, and we want to fix the constants in the equations
// representing the stays.  If both eplus and eminus are nonbasic
// they have value 0 in the current solution, meaning the previous
// stay was exactly satisfied.  In this case nothing needs to be
// changed.  Otherwise one of them is basic, and the other must
// occur only in the Expression for that basic error variable.
// Reset the Constant in this Expression to 0.
void
simplex_solver::reset_stay_constants()
{
    auto ip (stay_plus_error_vars_.begin());
    auto im (stay_minus_error_vars_.begin());

    for (; ip != stay_plus_error_vars_.end(); ++ip, ++im)
    {
        if (is_basic_var(*ip))
            row_expression(*ip).set_constant(0);
        else if (is_basic_var(*im))
            row_expression(*im).set_constant(0);
    }
}

// Set the external variables known to this solver to their appropriate values.
// Set each external basic variable to its value, and set each
// external parametric variable to 0.  (It isn't clear that we will
// ever have external parametric variables -- every external
// variable should either have a stay on it, or have an equation
// that defines it in terms of other external variables that do have
// stays.  For the moment I'll put this in though.)  Variables that
// are internal to the solver don't actually store values -- their
// values are just implicit in the tableu -- so we don't need to set
// them."
void
simplex_solver::set_external_variables()
{
    // Set external parametric variables first
    // in case I've screwed up
    for (variable v : external_parametric_vars_)
        change(v, 0);

    // Only iterate over the rows w/ external variables
    for (variable v : external_rows_)
        change(v, row_expression(v).constant());

    needs_solving_ = false;
    if (on_resolve)
        on_resolve(*this);
}

bool
simplex_solver::is_constraint_satisfied(const constraint_ref& c) const
{
    if (marker_vars_.count(c) == 0)
        throw constraint_not_found();

    auto ie (error_vars_.find(c));
    if (ie != error_vars_.end())
    {
        for (variable v : ie->second)
        {
            if (is_parametric_var(v))
                continue;

            if (!near_zero(row_expression(v).constant()))
                return false;
        }
    }
    return true;
}

simplex_solver&
simplex_solver::remove_constraint(const constraint_ref& c)
{
    remove_constraint_internal(c);
    c->removed_from(*this);
    return *this;
}

void
simplex_solver::change_strength_and_weight(constraint_ref c,
                                           const strength& s,
                                           double weight)
{
    auto ie (error_vars_.find(c));
    if (ie == error_vars_.end())
        return;

    double old_coeff (c->adjusted_symbolic_weight());
    c->set_strength(s);
    c->set_weight(weight);
    double new_coeff (c->adjusted_symbolic_weight());

    if (new_coeff == old_coeff)
        return;

    auto& row (row_expression(objective_));
    for (const variable& v : ie->second)
    {
        if (!is_basic_var(v))
        {
            row.add(v, -old_coeff, objective_, *this);
            row.add(v, new_coeff, objective_, *this);
        }
        else
        {
            const linear_expression& expr (row_expression(v));
            row.add(expr * -old_coeff, objective_, *this);
            row.add(expr * new_coeff, objective_, *this);
        }
    }
    needs_solving_ = true;

    if (auto_solve_)
    {
        optimize(objective_);
        set_external_variables();
    }
}

void simplex_solver::change_strength(constraint_ref c, const strength& s)
{
    change_strength_and_weight(c, s, c->weight());
}

void simplex_solver::change_weight(constraint_ref c, double weight)
{
    change_strength_and_weight(c, c->get_strength(), weight);
}

} // namespace rhea
