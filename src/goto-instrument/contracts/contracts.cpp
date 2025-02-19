/*******************************************************************\

Module: Verify and use annotated loop and function contracts

Author: Michael Tautschnig

Date: February 2016

\*******************************************************************/

/// \file
/// Verify and use annotated loop and function contracts

#include "contracts.h"

#include <util/c_types.h>
#include <util/exception_utils.h>
#include <util/expr_util.h>
#include <util/find_symbols.h>
#include <util/format_expr.h>
#include <util/fresh_symbol.h>
#include <util/graph.h>
#include <util/mathematical_expr.h>
#include <util/message.h>
#include <util/std_code.h>

#include <goto-programs/goto_inline.h>
#include <goto-programs/goto_program.h>
#include <goto-programs/remove_skip.h>

#include <analyses/local_may_alias.h>
#include <ansi-c/c_expr.h>
#include <goto-instrument/havoc_utils.h>
#include <goto-instrument/nondet_static.h>
#include <goto-instrument/unwind.h>
#include <goto-instrument/unwindset.h>
#include <langapi/language_util.h>

#include "cfg_info.h"
#include "havoc_assigns_clause_targets.h"
#include "inlining_decorator.h"
#include "instrument_spec_assigns.h"
#include "memory_predicates.h"
#include "utils.h"

#include <algorithm>
#include <map>

void code_contractst::check_apply_loop_contracts(
  const irep_idt &function_name,
  goto_functionst::goto_functiont &goto_function,
  const local_may_aliast &local_may_alias,
  goto_programt::targett loop_head,
  goto_programt::targett loop_end,
  const loopt &loop,
  exprt assigns_clause,
  exprt invariant,
  exprt decreases_clause,
  const irep_idt &mode)
{
  const auto loop_head_location = loop_head->source_location();
  const auto loop_number = loop_end->loop_number;

  // Vector representing a (possibly multidimensional) decreases clause
  const auto &decreases_clause_exprs = decreases_clause.operands();

  // Temporary variables for storing the multidimensional decreases clause
  // at the start of and end of a loop body
  std::vector<symbol_exprt> old_decreases_vars, new_decreases_vars;

  // replace bound variables by fresh instances
  if(has_subexpr(invariant, ID_exists) || has_subexpr(invariant, ID_forall))
    add_quantified_variable(symbol_table, invariant, mode);

  // instrument
  //
  //   ... preamble ...
  // HEAD:
  //   ... eval guard ...
  //   if (!guard)
  //     goto EXIT;
  //   ... loop body ...
  //   goto HEAD;
  // EXIT:
  //   ... postamble ...
  //
  // to
  //
  //                               ... preamble ...
  //                        ,-     initialize loop_entry history vars;
  //                        |      entered_loop = false
  // loop assigns check     |      initial_invariant_val = invariant_expr;
  //  - unchecked, temps    |      in_base_case = true;
  // func assigns check     |      snapshot (write_set);
  //  - disabled via pragma |      goto HEAD;
  //                        |    STEP:
  //                  --.   |      assert (initial_invariant_val);
  // loop assigns check |   |      in_base_case = false;
  //  - not applicable   >=======  in_loop_havoc_block = true;
  // func assigns check |   |      havoc (assigns_set);
  //  + deferred        |   |      in_loop_havoc_block = false;
  //                  --'   |      assume (invariant_expr);
  //                        `-     old_variant_val = decreases_clause_expr;
  //                           * HEAD:
  // loop assigns check     ,-     ... eval guard ...
  //  + assertions added    |      if (!guard)
  // func assigns check     |        goto EXIT;
  //  - disabled via pragma `-     ... loop body ...
  //                        ,-     entered_loop = true
  //                        |      if (in_base_case)
  //                        |        goto STEP;
  // loop assigns check     |      assert (invariant_expr);
  //  - unchecked, temps    |      new_variant_val = decreases_clause_expr;
  // func assigns check     |      assert (new_variant_val < old_variant_val);
  //  - disabled via pragma |      dead old_variant_val, new_variant_val;
  //                        |  *   assume (false);
  //                        |  * EXIT:
  // To be inserted at  ,~~~|~~~~  assert (entered_loop ==> !in_base_case);
  // every unique EXIT  |   |      dead loop_entry history vars, in_base_case;
  // (break, goto etc.) `~~~`- ~~  dead initial_invariant_val, entered_loop;
  //                               ... postamble ..
  //
  // Asterisks (*) denote anchor points (goto targets) for instrumentations.
  // We insert generated code above and/below these targets.
  //
  // Assertions for assigns clause checking are inserted in the loop body.

  // Process "loop_entry" history variables.
  // We find and replace all "__CPROVER_loop_entry" subexpressions in invariant.
  auto replace_history_result = replace_history_loop_entry(
    symbol_table, invariant, loop_head_location, mode);
  invariant.swap(replace_history_result.expression_after_replacement);
  const auto &history_var_map = replace_history_result.parameter_to_history;
  // an intermediate goto_program to store generated instructions
  // to be inserted before the loop head
  goto_programt &pre_loop_head_instrs =
    replace_history_result.history_construction;

  // Create a temporary to track if we entered the loop,
  // i.e., the loop guard was satisfied.
  const auto entered_loop =
    get_fresh_aux_symbol(
      bool_typet(),
      id2string(loop_head_location.get_function()),
      std::string(ENTERED_LOOP) + "__" + std::to_string(loop_number),
      loop_head_location,
      mode,
      symbol_table)
      .symbol_expr();
  pre_loop_head_instrs.add(
    goto_programt::make_decl(entered_loop, loop_head_location));
  pre_loop_head_instrs.add(
    goto_programt::make_assignment(entered_loop, false_exprt{}));

  // Create a snapshot of the invariant so that we can check the base case,
  // if the loop is not vacuous and must be abstracted with contracts.
  const auto initial_invariant_val =
    get_fresh_aux_symbol(
      bool_typet(),
      id2string(loop_head_location.get_function()),
      INIT_INVARIANT,
      loop_head_location,
      mode,
      symbol_table)
      .symbol_expr();
  pre_loop_head_instrs.add(
    goto_programt::make_decl(initial_invariant_val, loop_head_location));
  {
    // Although the invariant at this point will not have side effects,
    // it is still a C expression, and needs to be "goto_convert"ed.
    // Note that this conversion may emit many GOTO instructions.
    code_assignt initial_invariant_value_assignment{
      initial_invariant_val, invariant};
    initial_invariant_value_assignment.add_source_location() =
      loop_head_location;

    goto_convertt converter(symbol_table, log.get_message_handler());
    converter.goto_convert(
      initial_invariant_value_assignment, pre_loop_head_instrs, mode);
  }

  // Create a temporary variable to track base case vs inductive case
  // instrumentation of the loop.
  const auto in_base_case = get_fresh_aux_symbol(
                              bool_typet(),
                              id2string(loop_head_location.get_function()),
                              "__in_base_case",
                              loop_head_location,
                              mode,
                              symbol_table)
                              .symbol_expr();
  pre_loop_head_instrs.add(
    goto_programt::make_decl(in_base_case, loop_head_location));
  pre_loop_head_instrs.add(
    goto_programt::make_assignment(in_base_case, true_exprt{}));

  // CAR snapshot instructions for checking assigns clause
  goto_programt snapshot_instructions;

  loop_cfg_infot cfg_info(goto_function, loop);

  // track static local symbols
  instrument_spec_assignst instrument_spec_assigns(
    function_name,
    goto_functions,
    cfg_info,
    symbol_table,
    log.get_message_handler());

  instrument_spec_assigns.track_static_locals_between(
    loop_head, loop_end, snapshot_instructions);

  // set of targets to havoc
  assignst to_havoc;

  if(assigns_clause.is_nil())
  {
    // No assigns clause was specified for this loop.
    // Infer memory locations assigned by the loop from the loop instructions
    // and the inferred aliasing relation.
    try
    {
      infer_loop_assigns(local_may_alias, loop, to_havoc);

      // remove loop-local symbols from the inferred set
      cfg_info.erase_locals(to_havoc);

      // If the set contains pairs (i, a[i]),
      // we widen them to (i, __CPROVER_POINTER_OBJECT(a))
      widen_assigns(to_havoc, ns);

      log.debug() << "No loop assigns clause provided. Inferred targets: {";
      // Add inferred targets to the loop assigns clause.
      bool ran_once = false;
      for(const auto &target : to_havoc)
      {
        if(ran_once)
          log.debug() << ", ";
        ran_once = true;
        log.debug() << format(target);
        instrument_spec_assigns.track_spec_target(
          target, snapshot_instructions);
      }
      log.debug() << "}" << messaget::eom;

      instrument_spec_assigns.get_static_locals(
        std::inserter(to_havoc, to_havoc.end()));
    }
    catch(const analysis_exceptiont &exc)
    {
      log.error() << "Failed to infer variables being modified by the loop at "
                  << loop_head_location
                  << ".\nPlease specify an assigns clause.\nReason:"
                  << messaget::eom;
      throw exc;
    }
  }
  else
  {
    // An assigns clause was specified for this loop.
    // Add the targets to the set of expressions to havoc.
    for(const auto &target : assigns_clause.operands())
    {
      to_havoc.insert(target);
      instrument_spec_assigns.track_spec_target(target, snapshot_instructions);
    }
  }

  // Insert instrumentation
  // This must be done before havocing the write set.
  // FIXME: this is not true for write set targets that
  // might depend on other write set targets.
  pre_loop_head_instrs.destructive_append(snapshot_instructions);

  // Insert a jump to the loop head
  // (skipping over the step case initialization code below)
  pre_loop_head_instrs.add(
    goto_programt::make_goto(loop_head, true_exprt{}, loop_head_location));

  // The STEP case instructions follow.
  // We skip past it initially, because of the unconditional jump above,
  // but jump back here if we get past the loop guard while in_base_case.

  const auto step_case_target =
    pre_loop_head_instrs.add(goto_programt::make_assignment(
      in_base_case, false_exprt{}, loop_head_location));

  // If we jump here, then the loop runs at least once,
  // so add the base case assertion:
  //   assert(initial_invariant_val)
  // We use a block scope for assertion, since it's immediately goto converted,
  // and doesn't need to be kept around.
  {
    code_assertt assertion{initial_invariant_val};
    assertion.add_source_location() = loop_head_location;
    assertion.add_source_location().set_comment(
      "Check loop invariant before entry");

    goto_convertt converter(symbol_table, log.get_message_handler());
    converter.goto_convert(assertion, pre_loop_head_instrs, mode);
  }

  // Insert the first block of pre_loop_head_instrs,
  // with a pragma to disable assigns clause checking.
  // All of the initialization code so far introduces local temporaries,
  // which need not be checked against the enclosing scope's assigns clause.
  goto_function.body.destructive_insert(
    loop_head, add_pragma_disable_assigns_check(pre_loop_head_instrs));

  // Generate havocing code for assignment targets.
  // ASSIGN in_loop_havoc_block = true;
  // havoc (assigns_set);
  // ASSIGN in_loop_havoc_block = false;
  const auto in_loop_havoc_block =
    get_fresh_aux_symbol(
      bool_typet(),
      id2string(loop_head_location.get_function()),
      std::string(IN_LOOP_HAVOC_BLOCK) + +"__" + std::to_string(loop_number),
      loop_head_location,
      mode,
      symbol_table)
      .symbol_expr();
  pre_loop_head_instrs.add(
    goto_programt::make_decl(in_loop_havoc_block, loop_head_location));
  pre_loop_head_instrs.add(
    goto_programt::make_assignment(in_loop_havoc_block, true_exprt{}));
  havoc_assigns_targetst havoc_gen(
    to_havoc, symbol_table, log.get_message_handler(), mode);
  havoc_gen.append_full_havoc_code(loop_head_location, pre_loop_head_instrs);
  pre_loop_head_instrs.add(
    goto_programt::make_assignment(in_loop_havoc_block, false_exprt{}));

  // Insert the second block of pre_loop_head_instrs: the havocing code.
  // We do not `add_pragma_disable_assigns_check`,
  // so that the enclosing scope's assigns clause instrumentation
  // would pick these havocs up for inclusion (subset) checks.
  goto_function.body.destructive_insert(loop_head, pre_loop_head_instrs);

  // Generate: assume(invariant) just after havocing
  // We use a block scope for assumption, since it's immediately goto converted,
  // and doesn't need to be kept around.
  {
    code_assumet assumption{invariant};
    assumption.add_source_location() = loop_head_location;

    goto_convertt converter(symbol_table, log.get_message_handler());
    converter.goto_convert(assumption, pre_loop_head_instrs, mode);
  }

  // Create fresh temporary variables that store the multidimensional
  // decreases clause's value before and after the loop
  for(const auto &clause : decreases_clause.operands())
  {
    const auto old_decreases_var =
      get_fresh_aux_symbol(
        clause.type(),
        id2string(loop_head_location.get_function()),
        "tmp_cc",
        loop_head_location,
        mode,
        symbol_table)
        .symbol_expr();
    pre_loop_head_instrs.add(
      goto_programt::make_decl(old_decreases_var, loop_head_location));
    old_decreases_vars.push_back(old_decreases_var);

    const auto new_decreases_var =
      get_fresh_aux_symbol(
        clause.type(),
        id2string(loop_head_location.get_function()),
        "tmp_cc",
        loop_head_location,
        mode,
        symbol_table)
        .symbol_expr();
    pre_loop_head_instrs.add(
      goto_programt::make_decl(new_decreases_var, loop_head_location));
    new_decreases_vars.push_back(new_decreases_var);
  }

  // TODO: Fix loop contract handling for do/while loops.
  if(loop_end->is_goto() && !loop_end->condition().is_true())
  {
    log.error() << "Loop contracts are unsupported on do/while loops: "
                << loop_head_location << messaget::eom;
    throw 0;

    // non-deterministically skip the loop if it is a do-while loop.
    // pre_loop_head_instrs.add(goto_programt::make_goto(
    //   loop_end, side_effect_expr_nondett(bool_typet(), loop_head_location)));
  }

  // Generate: assignments to store the multidimensional decreases clause's
  // value just before the loop_head
  if(!decreases_clause.is_nil())
  {
    for(size_t i = 0; i < old_decreases_vars.size(); i++)
    {
      code_assignt old_decreases_assignment{
        old_decreases_vars[i], decreases_clause_exprs[i]};
      old_decreases_assignment.add_source_location() = loop_head_location;

      goto_convertt converter(symbol_table, log.get_message_handler());
      converter.goto_convert(
        old_decreases_assignment, pre_loop_head_instrs, mode);
    }

    goto_function.body.destructive_insert(
      loop_head, add_pragma_disable_assigns_check(pre_loop_head_instrs));
  }

  // Insert the third and final block of pre_loop_head_instrs,
  // with a pragma to disable assigns clause checking.
  // The variables to store old variant value are local temporaries,
  // which need not be checked against the enclosing scope's assigns clause.
  goto_function.body.destructive_insert(
    loop_head, add_pragma_disable_assigns_check(pre_loop_head_instrs));

  // Perform write set instrumentation on the entire loop.
  instrument_spec_assigns.instrument_instructions(
    goto_function.body,
    loop_head,
    loop_end,
    [&loop](const goto_programt::targett &t) { return loop.contains(t); });

  // Now we begin instrumenting at the loop_end.
  // `pre_loop_end_instrs` are to be inserted before `loop_end`.
  goto_programt pre_loop_end_instrs;

  // Record that we entered the loop.
  pre_loop_end_instrs.add(
    goto_programt::make_assignment(entered_loop, true_exprt{}));

  // Jump back to the step case to havoc the write set, assume the invariant,
  // and execute an arbitrary iteration.
  pre_loop_end_instrs.add(goto_programt::make_goto(
    step_case_target, in_base_case, loop_head_location));

  // The following code is only reachable in the step case,
  // i.e., when in_base_case == false,
  // because of the unconditional jump above.

  // Generate the inductiveness check:
  //   assert(invariant)
  // We use a block scope for assertion, since it's immediately goto converted,
  // and doesn't need to be kept around.
  {
    code_assertt assertion{invariant};
    assertion.add_source_location() = loop_head_location;
    assertion.add_source_location().set_comment(
      "Check that loop invariant is preserved");

    goto_convertt converter(symbol_table, log.get_message_handler());
    converter.goto_convert(assertion, pre_loop_end_instrs, mode);
  }

  // Generate: assignments to store the multidimensional decreases clause's
  // value after one iteration of the loop
  if(!decreases_clause.is_nil())
  {
    for(size_t i = 0; i < new_decreases_vars.size(); i++)
    {
      code_assignt new_decreases_assignment{
        new_decreases_vars[i], decreases_clause_exprs[i]};
      new_decreases_assignment.add_source_location() = loop_head_location;

      goto_convertt converter(symbol_table, log.get_message_handler());
      converter.goto_convert(
        new_decreases_assignment, pre_loop_end_instrs, mode);
    }

    // Generate: assertion that the multidimensional decreases clause's value
    // after the loop is lexicographically smaller than its initial value.
    code_assertt monotonic_decreasing_assertion{
      generate_lexicographic_less_than_check(
        new_decreases_vars, old_decreases_vars)};
    monotonic_decreasing_assertion.add_source_location() = loop_head_location;
    monotonic_decreasing_assertion.add_source_location().set_comment(
      "Check decreases clause on loop iteration");

    goto_convertt converter(symbol_table, log.get_message_handler());
    converter.goto_convert(
      monotonic_decreasing_assertion, pre_loop_end_instrs, mode);

    // Discard the temporary variables that store decreases clause's value
    for(size_t i = 0; i < old_decreases_vars.size(); i++)
    {
      pre_loop_end_instrs.add(
        goto_programt::make_dead(old_decreases_vars[i], loop_head_location));
      pre_loop_end_instrs.add(
        goto_programt::make_dead(new_decreases_vars[i], loop_head_location));
    }
  }

  insert_before_swap_and_advance(
    goto_function.body,
    loop_end,
    add_pragma_disable_assigns_check(pre_loop_end_instrs));

  // change the back edge into assume(false) or assume(guard)
  loop_end->turn_into_assume();
  loop_end->condition_nonconst() = boolean_negate(loop_end->condition());

  std::set<goto_programt::targett, goto_programt::target_less_than>
    seen_targets;
  // Find all exit points of the loop, make temporary variables `DEAD`,
  // and check that step case was checked for non-vacuous loops.
  for(const auto &t : loop)
  {
    if(!t->is_goto())
      continue;

    auto exit_target = t->get_target();
    if(
      loop.contains(exit_target) ||
      seen_targets.find(exit_target) != seen_targets.end())
      continue;

    seen_targets.insert(exit_target);

    goto_programt pre_loop_exit_instrs;
    // Assertion to check that step case was checked if we entered the loop.
    source_locationt annotated_location = loop_head_location;
    annotated_location.set_comment(
      "Check that loop instrumentation was not truncated");
    pre_loop_exit_instrs.add(goto_programt::make_assertion(
      or_exprt{not_exprt{entered_loop}, not_exprt{in_base_case}},
      annotated_location));
    // Instructions to make all the temporaries go dead.
    pre_loop_exit_instrs.add(
      goto_programt::make_dead(in_base_case, loop_head_location));
    pre_loop_exit_instrs.add(
      goto_programt::make_dead(initial_invariant_val, loop_head_location));
    for(const auto &v : history_var_map)
    {
      pre_loop_exit_instrs.add(
        goto_programt::make_dead(to_symbol_expr(v.second), loop_head_location));
    }
    // Insert these instructions, preserving the loop end target.
    insert_before_swap_and_advance(
      goto_function.body, exit_target, pre_loop_exit_instrs);
  }
}

/// Throws an exception if a contract uses unsupported constructs like:
/// - obeys_contract predicates
static void throw_on_unsupported(const goto_programt &program)
{
  for(const auto &it : program.instructions)
  {
    if(
      it.is_function_call() && it.call_function().id() == ID_symbol &&
      to_symbol_expr(it.call_function()).get_identifier() == CPROVER_PREFIX
        "obeys_contract")
    {
      throw invalid_source_file_exceptiont(
        CPROVER_PREFIX "obeys_contract is not supported in this version",
        it.source_location());
    }
  }
}

/// This function generates instructions for all contract constraint, i.e.,
/// assumptions and assertions based on requires and ensures clauses.
static void generate_contract_constraints(
  symbol_tablet &symbol_table,
  goto_convertt &converter,
  exprt &instantiated_clause,
  const irep_idt &mode,
  const std::function<void(goto_programt &)> &is_fresh_update,
  goto_programt &program,
  const source_locationt &location)
{
  if(
    has_subexpr(instantiated_clause, ID_exists) ||
    has_subexpr(instantiated_clause, ID_forall))
  {
    add_quantified_variable(symbol_table, instantiated_clause, mode);
  }

  goto_programt constraint;
  if(location.get_property_class() == ID_assume)
  {
    code_assumet assumption(instantiated_clause);
    assumption.add_source_location() = location;
    converter.goto_convert(assumption, constraint, mode);
  }
  else
  {
    code_assertt assertion(instantiated_clause);
    assertion.add_source_location() = location;
    converter.goto_convert(assertion, constraint, mode);
  }
  is_fresh_update(constraint);
  throw_on_unsupported(constraint);
  program.destructive_append(constraint);
}

static const code_with_contract_typet &
get_contract(const irep_idt &function, const namespacet &ns)
{
  const std::string &function_str = id2string(function);
  const auto &function_symbol = ns.lookup(function);

  const symbolt *contract_sym;
  if(ns.lookup("contract::" + function_str, contract_sym))
  {
    // no contract provided in the source or just an empty assigns clause
    return to_code_with_contract_type(function_symbol.type);
  }

  const auto &type = to_code_with_contract_type(contract_sym->type);
  DATA_INVARIANT(
    type == function_symbol.type,
    "front-end should have rejected re-declarations with a different type");

  return type;
}

void code_contractst::apply_function_contract(
  const irep_idt &function,
  const source_locationt &location,
  goto_programt &function_body,
  goto_programt::targett &target)
{
  const auto &const_target =
    static_cast<const goto_programt::targett &>(target);
  // Return if the function is not named in the call; currently we don't handle
  // function pointers.
  PRECONDITION(const_target->call_function().id() == ID_symbol);

  const irep_idt &target_function =
    to_symbol_expr(const_target->call_function()).get_identifier();
  const symbolt &function_symbol = ns.lookup(target_function);
  const code_typet &function_type = to_code_type(function_symbol.type);

  // Isolate each component of the contract.
  const auto &type = get_contract(target_function, ns);

  // Prepare to instantiate expressions in the callee
  // with expressions from the call site (e.g. the return value).
  exprt::operandst instantiation_values;

  // keep track of the call's return expression to make it nondet later
  std::optional<exprt> call_ret_opt = {};

  // if true, the call return variable variable was created during replacement
  bool call_ret_is_fresh_var = false;

  if(function_type.return_type() != empty_typet())
  {
    // Check whether the function's return value is not disregarded.
    if(target->call_lhs().is_not_nil())
    {
      // If so, have it replaced appropriately.
      // For example, if foo() ensures that its return value is > 5, then
      // rewrite calls to foo as follows:
      // x = foo() -> assume(__CPROVER_return_value > 5) -> assume(x > 5)
      auto &lhs_expr = const_target->call_lhs();
      call_ret_opt = lhs_expr;
      instantiation_values.push_back(lhs_expr);
    }
    else
    {
      // If the function does return a value, but the return value is
      // disregarded, check if the postcondition includes the return value.
      if(std::any_of(
           type.c_ensures().begin(),
           type.c_ensures().end(),
           [](const exprt &e) {
             return has_symbol_expr(
               to_lambda_expr(e).where(), CPROVER_PREFIX "return_value", true);
           }))
      {
        // The postcondition does mention __CPROVER_return_value, so mint a
        // fresh variable to replace __CPROVER_return_value with.
        call_ret_is_fresh_var = true;
        const symbolt &fresh = get_fresh_aux_symbol(
          function_type.return_type(),
          id2string(target_function),
          "ignored_return_value",
          const_target->source_location(),
          symbol_table.lookup_ref(target_function).mode,
          ns,
          symbol_table);
        auto fresh_sym_expr = fresh.symbol_expr();
        call_ret_opt = fresh_sym_expr;
        instantiation_values.push_back(fresh_sym_expr);
      }
      else
      {
        // unused, add a dummy with the right type
        instantiation_values.push_back(
          exprt{ID_nil, function_type.return_type()});
      }
    }
  }

  // Replace formal parameters
  const auto &arguments = const_target->call_arguments();
  instantiation_values.insert(
    instantiation_values.end(), arguments.begin(), arguments.end());

  const auto &mode = function_symbol.mode;

  // new program where all contract-derived instructions are added
  goto_programt new_program;

  is_fresh_replacet is_fresh(
    goto_model, log.get_message_handler(), target_function);
  is_fresh.create_declarations();
  is_fresh.add_memory_map_decl(new_program);

  // Generate: assert(requires)
  for(const auto &clause : type.c_requires())
  {
    auto instantiated_clause =
      to_lambda_expr(clause).application(instantiation_values);
    source_locationt _location = clause.source_location();
    _location.set_line(location.get_line());
    _location.set_comment(std::string("Check requires clause of ")
                            .append(target_function.c_str())
                            .append(" in ")
                            .append(function.c_str()));
    _location.set_property_class(ID_precondition);
    goto_convertt converter(symbol_table, log.get_message_handler());
    generate_contract_constraints(
      symbol_table,
      converter,
      instantiated_clause,
      mode,
      [&is_fresh](goto_programt &c_requires) {
        is_fresh.update_requires(c_requires);
      },
      new_program,
      _location);
  }

  // Generate all the instructions required to initialize history variables
  exprt::operandst instantiated_ensures_clauses;
  for(auto clause : type.c_ensures())
  {
    auto instantiated_clause =
      to_lambda_expr(clause).application(instantiation_values);
    instantiated_clause.add_source_location() = clause.source_location();
    generate_history_variables_initialization(
      symbol_table, instantiated_clause, mode, new_program);
    instantiated_ensures_clauses.push_back(instantiated_clause);
  }

  // ASSIGNS clause should not refer to any quantified variables,
  // and only refer to the common symbols to be replaced.
  exprt::operandst targets;
  for(auto &target : type.c_assigns())
    targets.push_back(to_lambda_expr(target).application(instantiation_values));

  // Create a sequence of non-deterministic assignments ...

  // ... for the assigns clause targets and static locals
  goto_programt havoc_instructions;
  function_cfg_infot cfg_info({});
  havoc_assigns_clause_targetst havocker(
    target_function,
    targets,
    goto_functions,
    cfg_info,
    location,
    symbol_table,
    log.get_message_handler());

  havocker.get_instructions(havoc_instructions);

  // ... for the return value
  if(call_ret_opt.has_value())
  {
    auto &call_ret = call_ret_opt.value();
    auto &loc = call_ret.source_location();
    auto &type = call_ret.type();

    // Declare if fresh
    if(call_ret_is_fresh_var)
      havoc_instructions.add(
        goto_programt::make_decl(to_symbol_expr(call_ret), loc));

    side_effect_expr_nondett expr(type, location);
    havoc_instructions.add(goto_programt::make_assignment(
      code_assignt{call_ret, std::move(expr), loc}, loc));
  }

  // Insert havoc instructions immediately before the call site.
  new_program.destructive_append(havoc_instructions);

  // Generate: assume(ensures)
  for(auto &clause : instantiated_ensures_clauses)
  {
    if(clause.is_false())
    {
      throw invalid_input_exceptiont(
        std::string("Attempt to assume false at ")
          .append(clause.source_location().as_string())
          .append(".\nPlease update ensures clause to avoid vacuity."));
    }
    source_locationt _location = clause.source_location();
    _location.set_comment("Assume ensures clause");
    _location.set_property_class(ID_assume);

    goto_convertt converter(symbol_table, log.get_message_handler());
    generate_contract_constraints(
      symbol_table,
      converter,
      clause,
      mode,
      [&is_fresh](goto_programt &ensures) { is_fresh.update_ensures(ensures); },
      new_program,
      _location);
  }

  // Kill return value variable if fresh
  if(call_ret_is_fresh_var)
  {
    log.conditional_output(
      log.warning(), [&target](messaget::mstreamt &mstream) {
        target->output(mstream);
        mstream << messaget::eom;
      });
    auto dead_inst =
      goto_programt::make_dead(to_symbol_expr(call_ret_opt.value()), location);
    function_body.insert_before_swap(target, dead_inst);
    ++target;
  }

  is_fresh.add_memory_map_dead(new_program);

  // Erase original function call
  *target = goto_programt::make_skip();

  // insert contract replacement instructions
  insert_before_swap_and_advance(function_body, target, new_program);

  // Add this function to the set of replaced functions.
  summarized.insert(target_function);

  // restore global goto_program consistency
  goto_functions.update();
}

void code_contractst::apply_loop_contract(
  const irep_idt &function_name,
  goto_functionst::goto_functiont &goto_function)
{
  const bool may_have_loops = std::any_of(
    goto_function.body.instructions.begin(),
    goto_function.body.instructions.end(),
    [](const goto_programt::instructiont &instruction) {
      return instruction.is_backwards_goto();
    });

  if(!may_have_loops)
    return;

  inlining_decoratort decorated(log.get_message_handler());
  goto_function_inline(
    goto_functions, function_name, ns, log.get_message_handler());

  INVARIANT(
    decorated.get_recursive_call_set().size() == 0,
    "Recursive functions found during inlining");

  // restore internal invariants
  goto_functions.update();

  local_may_aliast local_may_alias(goto_function);
  natural_loops_mutablet natural_loops(goto_function.body);

  // A graph node type that stores information about a loop.
  // We create a DAG representing nesting of various loops in goto_function,
  // sort them topologically, and instrument them in the top-sorted order.
  //
  // The goal here is not avoid explicit "subset checks" on nested write sets.
  // When instrumenting in a top-sorted order,
  // the outer loop would no longer observe the inner loop's write set,
  // but only corresponding `havoc` statements,
  // which can be instrumented in the usual way to achieve a "subset check".

  struct loop_graph_nodet : public graph_nodet<empty_edget>
  {
    const typename natural_loops_mutablet::loopt &content;
    const goto_programt::targett head_target, end_target;
    exprt assigns_clause, invariant, decreases_clause;

    loop_graph_nodet(
      const typename natural_loops_mutablet::loopt &loop,
      const goto_programt::targett head,
      const goto_programt::targett end,
      const exprt &assigns,
      const exprt &inv,
      const exprt &decreases)
      : content(loop),
        head_target(head),
        end_target(end),
        assigns_clause(assigns),
        invariant(inv),
        decreases_clause(decreases)
    {
    }
  };
  grapht<loop_graph_nodet> loop_nesting_graph;

  std::list<size_t> to_check_contracts_on_children;

  std::map<
    goto_programt::targett,
    std::pair<goto_programt::targett, natural_loops_mutablet::loopt>,
    goto_programt::target_less_than>
    loop_head_ends;

  for(const auto &loop_head_and_content : natural_loops.loop_map)
  {
    const auto &loop_body = loop_head_and_content.second;
    // Skip empty loops and self-looped node.
    if(loop_body.size() <= 1)
      continue;

    auto loop_head = loop_head_and_content.first;
    auto loop_end = loop_head;

    // Find the last back edge to `loop_head`
    for(const auto &t : loop_body)
    {
      if(
        t->is_goto() && t->get_target() == loop_head &&
        t->location_number > loop_end->location_number)
        loop_end = t;
    }

    if(loop_end == loop_head)
    {
      log.error() << "Could not find end of the loop starting at: "
                  << loop_head->source_location() << messaget::eom;
      throw 0;
    }

    // By definition the `loop_body` is a set of instructions computed
    // by `natural_loops` based on the CFG.
    // Since we perform assigns clause instrumentation by sequentially
    // traversing instructions from `loop_head` to `loop_end`,
    // here we ensure that all instructions in `loop_body` belong within
    // the [loop_head, loop_end] target range.

    // Check 1. (i \in loop_body) ==> loop_head <= i <= loop_end
    for(const auto &i : loop_body)
    {
      if(
        loop_head->location_number > i->location_number ||
        i->location_number > loop_end->location_number)
      {
        log.conditional_output(
          log.error(), [&i, &loop_head](messaget::mstreamt &mstream) {
            mstream << "Computed loop at " << loop_head->source_location()
                    << "contains an instruction beyond [loop_head, loop_end]:"
                    << messaget::eom;
            i->output(mstream);
            mstream << messaget::eom;
          });
        throw 0;
      }
    }

    if(!loop_head_ends.emplace(loop_head, std::make_pair(loop_end, loop_body))
          .second)
      UNREACHABLE;
  }

  for(auto &loop_head_end : loop_head_ends)
  {
    auto loop_head = loop_head_end.first;
    auto loop_end = loop_head_end.second.first;
    // After loop-contract instrumentation, jumps to the `loop_head` will skip
    // some instrumented instructions. So we want to make sure that there is
    // only one jump targeting `loop_head` from `loop_end` before loop-contract
    // instrumentation.
    // Add a skip before `loop_head` and let all jumps (except for the
    // `loop_end`) that target to the `loop_head` target to the skip
    // instead.
    insert_before_and_update_jumps(
      goto_function.body, loop_head, goto_programt::make_skip());
    loop_end->set_target(loop_head);

    exprt assigns_clause = get_loop_assigns(loop_end);
    exprt invariant =
      get_loop_invariants(loop_end, loop_contract_config.check_side_effect);
    exprt decreases_clause =
      get_loop_decreases(loop_end, loop_contract_config.check_side_effect);

    if(invariant.is_nil())
    {
      if(decreases_clause.is_not_nil() || assigns_clause.is_not_nil())
      {
        invariant = true_exprt{};
        // assigns clause is missing; we will try to automatic inference
        log.warning()
          << "The loop at " << loop_head->source_location().as_string()
          << " does not have an invariant in its contract.\n"
          << "Hence, a default invariant ('true') is being used.\n"
          << "This choice is sound, but verification may fail"
          << " if it is be too weak to prove the desired properties."
          << messaget::eom;
      }
    }
    else
    {
      invariant = conjunction(invariant.operands());
      if(decreases_clause.is_nil())
      {
        log.warning() << "The loop at "
                      << loop_head->source_location().as_string()
                      << " does not have a decreases clause in its contract.\n"
                      << "Termination of this loop will not be verified."
                      << messaget::eom;
      }
    }

    const auto idx = loop_nesting_graph.add_node(
      loop_head_end.second.second,
      loop_head,
      loop_end,
      assigns_clause,
      invariant,
      decreases_clause);

    if(
      assigns_clause.is_nil() && invariant.is_nil() &&
      decreases_clause.is_nil())
      continue;

    to_check_contracts_on_children.push_back(idx);
  }

  for(size_t outer = 0; outer < loop_nesting_graph.size(); ++outer)
  {
    for(size_t inner = 0; inner < loop_nesting_graph.size(); ++inner)
    {
      if(inner == outer)
        continue;

      if(loop_nesting_graph[outer].content.contains(
           loop_nesting_graph[inner].head_target))
      {
        if(!loop_nesting_graph[outer].content.contains(
             loop_nesting_graph[inner].end_target))
        {
          log.error()
            << "Overlapping loops at:\n"
            << loop_nesting_graph[outer].head_target->source_location()
            << "\nand\n"
            << loop_nesting_graph[inner].head_target->source_location()
            << "\nLoops must be nested or sequential for contracts to be "
               "enforced."
            << messaget::eom;
        }
        loop_nesting_graph.add_edge(inner, outer);
      }
    }
  }

  // make sure all children of a contractified loop also have contracts
  while(!to_check_contracts_on_children.empty())
  {
    const auto loop_idx = to_check_contracts_on_children.front();
    to_check_contracts_on_children.pop_front();

    const auto &loop_node = loop_nesting_graph[loop_idx];
    if(
      loop_node.assigns_clause.is_nil() && loop_node.invariant.is_nil() &&
      loop_node.decreases_clause.is_nil())
    {
      log.error()
        << "Inner loop at: " << loop_node.head_target->source_location()
        << " does not have contracts, but an enclosing loop does.\n"
        << "Please provide contracts for this loop, or unwind it first."
        << messaget::eom;
      throw 0;
    }

    for(const auto child_idx : loop_nesting_graph.get_predecessors(loop_idx))
      to_check_contracts_on_children.push_back(child_idx);
  }

  // Iterate over the (natural) loops in the function, in topo-sorted order,
  // and apply any loop contracts that we find.
  for(const auto &idx : loop_nesting_graph.topsort())
  {
    const auto &loop_node = loop_nesting_graph[idx];
    if(
      loop_node.assigns_clause.is_nil() && loop_node.invariant.is_nil() &&
      loop_node.decreases_clause.is_nil())
      continue;

    // Computed loop "contents" needs to be refreshed to include any newly
    // introduced instrumentation, e.g. havoc instructions for assigns clause,
    // on inner loops in to the outer loop's contents.
    // Else, during the outer loop instrumentation these instructions will be
    // "masked" just as any other instruction not within its "contents."

    goto_functions.update();
    natural_loops_mutablet updated_loops(goto_function.body);

    // We will unwind all transformed loops twice. Store the names of
    // to-unwind-loops here and perform the unwinding after transformation done.
    // As described in `check_apply_loop_contracts`, loops with loop contracts
    // will be transformed to a loop that execute exactly twice: one for base
    // case and one for step case. So we unwind them here to avoid users
    // incorrectly unwind them only once.
    std::string to_unwind = id2string(function_name) + "." +
                            std::to_string(loop_node.end_target->loop_number) +
                            ":2";
    loop_names.push_back(to_unwind);

    check_apply_loop_contracts(
      function_name,
      goto_function,
      local_may_alias,
      loop_node.head_target,
      loop_node.end_target,
      updated_loops.loop_map[loop_node.head_target],
      loop_node.assigns_clause,
      loop_node.invariant,
      loop_node.decreases_clause,
      symbol_table.lookup_ref(function_name).mode);
  }
}

void code_contractst::check_frame_conditions_function(const irep_idt &function)
{
  // Get the function object before instrumentation.
  auto function_obj = goto_functions.function_map.find(function);

  INVARIANT(
    function_obj != goto_functions.function_map.end(),
    "Function '" + id2string(function) + "'must exist in the goto program");

  const auto &goto_function = function_obj->second;
  auto &function_body = function_obj->second.body;

  function_cfg_infot cfg_info(goto_function);

  instrument_spec_assignst instrument_spec_assigns(
    function,
    goto_functions,
    cfg_info,
    symbol_table,
    log.get_message_handler());

  // Detect and track static local variables before inlining
  goto_programt snapshot_static_locals;
  instrument_spec_assigns.track_static_locals(snapshot_static_locals);

  // Full inlining of the function body
  // Inlining is performed so that function calls to a same function
  // occurring under different write sets get instrumented specifically
  // for each write set
  inlining_decoratort decorated(log.get_message_handler());
  goto_function_inline(goto_functions, function, ns, decorated);

  decorated.throw_on_recursive_calls(log, 0);

  // Clean up possible fake loops that are due to `IF 0!=0 GOTO i` instructions
  simplify_gotos(function_body, ns);

  // Restore internal coherence in the programs
  goto_functions.update();

  INVARIANT(
    is_loop_free(function_body, ns, log),
    "Loops remain in function '" + id2string(function) +
      "', assigns clause checking instrumentation cannot be applied.");

  // Start instrumentation
  auto instruction_it = function_body.instructions.begin();

  // Inject local static snapshots
  insert_before_swap_and_advance(
    function_body, instruction_it, snapshot_static_locals);

  // Track targets mentioned in the specification
  const symbolt &function_symbol = ns.lookup(function);
  const code_typet &function_type = to_code_type(function_symbol.type);
  exprt::operandst instantiation_values;
  // assigns clauses cannot refer to the return value, but we still need an
  // element in there to apply the lambda function consistently
  if(function_type.return_type() != empty_typet())
    instantiation_values.push_back(exprt{ID_nil, function_type.return_type()});
  for(const auto &param : function_type.parameters())
  {
    instantiation_values.push_back(
      ns.lookup(param.get_identifier()).symbol_expr());
  }
  for(auto &target : get_contract(function, ns).c_assigns())
  {
    goto_programt payload;
    instrument_spec_assigns.track_spec_target(
      to_lambda_expr(target).application(instantiation_values), payload);
    insert_before_swap_and_advance(function_body, instruction_it, payload);
  }

  // Track formal parameters
  goto_programt snapshot_function_parameters;
  for(const auto &param : function_type.parameters())
  {
    goto_programt payload;
    instrument_spec_assigns.track_stack_allocated(
      ns.lookup(param.get_identifier()).symbol_expr(), payload);
    insert_before_swap_and_advance(function_body, instruction_it, payload);
  }

  // Restore internal coherence in the programs
  goto_functions.update();

  // Insert write set inclusion checks.
  instrument_spec_assigns.instrument_instructions(
    function_body, instruction_it, function_body.instructions.end());
}

void code_contractst::enforce_contract(const irep_idt &function)
{
  // Add statements to the source function
  // to ensure assigns clause is respected.
  check_frame_conditions_function(function);

  // Rename source function
  std::stringstream ss;
  ss << CPROVER_PREFIX << "contracts_original_" << function;
  const irep_idt mangled(ss.str());
  const irep_idt original(function);

  auto old_function = goto_functions.function_map.find(original);
  INVARIANT(
    old_function != goto_functions.function_map.end(),
    "Function to replace must exist in the program.");

  std::swap(goto_functions.function_map[mangled], old_function->second);
  goto_functions.function_map.erase(old_function);

  // Place a new symbol with the mangled name into the symbol table
  source_locationt sl;
  sl.set_file("instrumented for code contracts");
  sl.set_line("0");
  const symbolt *original_sym = symbol_table.lookup(original);
  symbolt mangled_sym = *original_sym;
  mangled_sym.name = mangled;
  mangled_sym.base_name = mangled;
  mangled_sym.location = sl;
  const auto mangled_found = symbol_table.insert(std::move(mangled_sym));
  INVARIANT(
    mangled_found.second,
    "There should be no existing function called " + ss.str() +
      " in the symbol table because that name is a mangled name");

  // Insert wrapper function into goto_functions
  auto nexist_old_function = goto_functions.function_map.find(original);
  INVARIANT(
    nexist_old_function == goto_functions.function_map.end(),
    "There should be no function called " + id2string(function) +
      " in the function map because that function should have had its"
      " name mangled");

  auto mangled_fun = goto_functions.function_map.find(mangled);
  INVARIANT(
    mangled_fun != goto_functions.function_map.end(),
    "There should be a function called " + ss.str() +
      " in the function map because we inserted a fresh goto-program"
      " with that mangled name");

  goto_functiont &wrapper = goto_functions.function_map[original];
  wrapper.parameter_identifiers = mangled_fun->second.parameter_identifiers;
  wrapper.body.add(goto_programt::make_end_function());
  add_contract_check(original, mangled, wrapper.body);
}

void code_contractst::add_contract_check(
  const irep_idt &wrapper_function,
  const irep_idt &mangled_function,
  goto_programt &dest)
{
  PRECONDITION(!dest.instructions.empty());

  // build:
  // decl ret
  // decl parameter1 ...
  // decl history_parameter1 ... [optional]
  // assume(requires)  [optional]
  // ret=function(parameter1, ...)
  // assert(ensures)

  const auto &code_type = get_contract(wrapper_function, ns);
  goto_programt check;

  // prepare function call including all declarations
  const symbolt &function_symbol = ns.lookup(mangled_function);
  code_function_callt call(function_symbol.symbol_expr());

  // Prepare to instantiate expressions in the callee
  // with expressions from the call site (e.g. the return value).
  exprt::operandst instantiation_values;

  source_locationt source_location = function_symbol.location;
  // Set function in source location to original function
  source_location.set_function(wrapper_function);

  // decl ret
  std::optional<code_returnt> return_stmt;
  if(code_type.return_type() != empty_typet())
  {
    symbol_exprt r = get_fresh_aux_symbol(
                       code_type.return_type(),
                       id2string(source_location.get_function()),
                       "tmp_cc",
                       source_location,
                       function_symbol.mode,
                       symbol_table)
                       .symbol_expr();
    check.add(goto_programt::make_decl(r, source_location));

    call.lhs() = r;
    return_stmt = code_returnt(r);

    instantiation_values.push_back(r);
  }

  // decl parameter1 ...
  goto_functionst::function_mapt::iterator f_it =
    goto_functions.function_map.find(mangled_function);
  PRECONDITION(f_it != goto_functions.function_map.end());

  const goto_functionst::goto_functiont &gf = f_it->second;
  for(const auto &parameter : gf.parameter_identifiers)
  {
    PRECONDITION(!parameter.empty());
    const symbolt &parameter_symbol = ns.lookup(parameter);
    symbol_exprt p = get_fresh_aux_symbol(
                       parameter_symbol.type,
                       id2string(source_location.get_function()),
                       "tmp_cc",
                       source_location,
                       parameter_symbol.mode,
                       symbol_table)
                       .symbol_expr();
    check.add(goto_programt::make_decl(p, source_location));
    check.add(goto_programt::make_assignment(
      p, parameter_symbol.symbol_expr(), source_location));

    call.arguments().push_back(p);

    instantiation_values.push_back(p);
  }

  is_fresh_enforcet visitor(
    goto_model, log.get_message_handler(), wrapper_function);
  visitor.create_declarations();
  visitor.add_memory_map_decl(check);

  // Generate: assume(requires)
  for(const auto &clause : code_type.c_requires())
  {
    auto instantiated_clause =
      to_lambda_expr(clause).application(instantiation_values);
    if(instantiated_clause.is_false())
    {
      throw invalid_input_exceptiont(
        std::string("Attempt to assume false at ")
          .append(clause.source_location().as_string())
          .append(".\nPlease update requires clause to avoid vacuity."));
    }
    source_locationt _location = clause.source_location();
    _location.set_comment("Assume requires clause");
    _location.set_property_class(ID_assume);
    goto_convertt converter(symbol_table, log.get_message_handler());
    generate_contract_constraints(
      symbol_table,
      converter,
      instantiated_clause,
      function_symbol.mode,
      [&visitor](goto_programt &c_requires) {
        visitor.update_requires(c_requires);
      },
      check,
      _location);
  }

  // Generate all the instructions required to initialize history variables
  exprt::operandst instantiated_ensures_clauses;
  for(auto clause : code_type.c_ensures())
  {
    auto instantiated_clause =
      to_lambda_expr(clause).application(instantiation_values);
    instantiated_clause.add_source_location() = clause.source_location();
    generate_history_variables_initialization(
      symbol_table, instantiated_clause, function_symbol.mode, check);
    instantiated_ensures_clauses.push_back(instantiated_clause);
  }

  // ret=mangled_function(parameter1, ...)
  check.add(goto_programt::make_function_call(call, source_location));

  // Generate: assert(ensures)
  for(auto &clause : instantiated_ensures_clauses)
  {
    source_locationt _location = clause.source_location();
    _location.set_comment("Check ensures clause");
    _location.set_property_class(ID_postcondition);
    goto_convertt converter(symbol_table, log.get_message_handler());
    generate_contract_constraints(
      symbol_table,
      converter,
      clause,
      function_symbol.mode,
      [&visitor](goto_programt &ensures) { visitor.update_ensures(ensures); },
      check,
      _location);
  }

  if(code_type.return_type() != empty_typet())
  {
    check.add(goto_programt::make_set_return_value(
      return_stmt.value().return_value(), source_location));
  }

  // kill the is_fresh memory map
  visitor.add_memory_map_dead(check);

  // prepend the new code to dest
  dest.destructive_insert(dest.instructions.begin(), check);

  // restore global goto_program consistency
  goto_functions.update();
}

void code_contractst::check_all_functions_found(
  const std::set<std::string> &functions) const
{
  for(const auto &function : functions)
  {
    if(
      goto_functions.function_map.find(function) ==
      goto_functions.function_map.end())
    {
      throw invalid_input_exceptiont(
        "Function '" + function + "' was not found in the GOTO program.");
    }
  }
}

void code_contractst::replace_calls(const std::set<std::string> &to_replace)
{
  if(to_replace.empty())
    return;

  log.status() << "Replacing function calls with contracts" << messaget::eom;

  check_all_functions_found(to_replace);

  for(auto &goto_function : goto_functions.function_map)
  {
    Forall_goto_program_instructions(ins, goto_function.second.body)
    {
      if(ins->is_function_call())
      {
        if(ins->call_function().id() != ID_symbol)
          continue;

        const irep_idt &called_function =
          to_symbol_expr(ins->call_function()).get_identifier();
        auto found = std::find(
          to_replace.begin(), to_replace.end(), id2string(called_function));
        if(found == to_replace.end())
          continue;

        apply_function_contract(
          goto_function.first,
          ins->source_location(),
          goto_function.second.body,
          ins);
      }
    }
  }

  for(auto &goto_function : goto_functions.function_map)
    remove_skip(goto_function.second.body);

  goto_functions.update();
}

void code_contractst::apply_loop_contracts(
  const std::set<std::string> &to_exclude_from_nondet_init)
{
  for(auto &goto_function : goto_functions.function_map)
    apply_loop_contract(goto_function.first, goto_function.second);

  log.status() << "Adding nondeterministic initialization "
                  "of static/global variables."
               << messaget::eom;
  nondet_static(goto_model, to_exclude_from_nondet_init);

  // unwind all transformed loops twice.
  if(loop_contract_config.unwind_transformed_loops)
  {
    unwindsett unwindset{goto_model};
    unwindset.parse_unwindset(loop_names, log.get_message_handler());
    goto_unwindt goto_unwind;
    goto_unwind(goto_model, unwindset, goto_unwindt::unwind_strategyt::ASSUME);
  }

  remove_skip(goto_model);

  // Record original loop number for some instrumented instructions.
  for(auto &goto_function_entry : goto_functions.function_map)
  {
    auto &goto_function = goto_function_entry.second;
    bool is_in_loop_havoc_block = false;

    unsigned loop_number_of_loop_havoc = 0;
    for(goto_programt::const_targett it_instr =
          goto_function.body.instructions.begin();
        it_instr != goto_function.body.instructions.end();
        it_instr++)
    {
      // Don't override original loop numbers.
      if(original_loop_number_map.count(it_instr) != 0)
        continue;

      // Store loop number for two type of instrumented instructions.
      // ASSIGN ENTERED_LOOP = false  ---   head of transformed loops.
      // ASSIGN ENTERED_LOOP = true   ---   end of transformed loops.
      if(
        is_transformed_loop_end(it_instr) || is_transformed_loop_head(it_instr))
      {
        const auto &assign_lhs =
          expr_try_dynamic_cast<symbol_exprt>(it_instr->assign_lhs());
        original_loop_number_map[it_instr] = get_suffix_unsigned(
          id2string(assign_lhs->get_identifier()),
          std::string(ENTERED_LOOP) + "__");
        continue;
      }

      // Loop havocs are assignments between
      // ASSIGN IN_LOOP_HAVOC_BLOCK = true
      // and
      // ASSIGN IN_LOOP_HAVOC_BLOCK = false

      // Entering the loop-havoc block.
      if(is_assignment_to_instrumented_variable(it_instr, IN_LOOP_HAVOC_BLOCK))
      {
        is_in_loop_havoc_block = it_instr->assign_rhs() == true_exprt();
        const auto &assign_lhs =
          expr_try_dynamic_cast<symbol_exprt>(it_instr->assign_lhs());
        loop_number_of_loop_havoc = get_suffix_unsigned(
          id2string(assign_lhs->get_identifier()),
          std::string(IN_LOOP_HAVOC_BLOCK) + "__");
        continue;
      }

      // Assignments in loop-havoc block are loop havocs.
      if(is_in_loop_havoc_block && it_instr->is_assign())
      {
        loop_havoc_set.emplace(it_instr);

        // Store loop number for loop havoc.
        original_loop_number_map[it_instr] = loop_number_of_loop_havoc;
      }
    }
  }
}

void code_contractst::enforce_contracts(
  const std::set<std::string> &to_enforce,
  const std::set<std::string> &to_exclude_from_nondet_init)
{
  if(to_enforce.empty())
    return;

  log.status() << "Enforcing contracts" << messaget ::eom;

  check_all_functions_found(to_enforce);

  for(const auto &function : to_enforce)
    enforce_contract(function);

  log.status() << "Adding nondeterministic initialization "
                  "of static/global variables."
               << messaget::eom;
  nondet_static(goto_model, to_exclude_from_nondet_init);
}
