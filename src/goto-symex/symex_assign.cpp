/*******************************************************************\

Module: Symbolic Execution

Author: Daniel Kroening, kroening@kroening.com

\*******************************************************************/

/// \file
/// Symbolic Execution

#include "symex_assign.h"

#include <util/byte_operators.h>
#include <util/expr_util.h>
#include <util/pointer_expr.h>
#include <util/range.h>

#include "expr_skeleton.h"
#include "goto_symex_state.h"
#include "symex_config.h"

// We can either use with_exprt or update_exprt when building expressions that
// modify components of an array or a struct. Set USE_UPDATE to use
// update_exprt.
// #define USE_UPDATE

constexpr bool use_update()
{
#ifdef USE_UPDATE
  return true;
#else
  return false;
#endif
}

/// Determine whether the RHS expression is a string constant initialization
/// \param rhs The RHS expression
/// \return True if the expression points to the first character of a string
///    constant
static bool is_string_constant_initialization(const exprt &rhs)
{
  if(const auto &address_of = expr_try_dynamic_cast<address_of_exprt>(rhs))
  {
    if(
      const auto &index =
        expr_try_dynamic_cast<index_exprt>(address_of->object()))
    {
      if(index->array().id() == ID_string_constant && index->index().is_zero())
      {
        return true;
      }
    }
  }
  return false;
}

void symex_assignt::assign_rec(
  const exprt &lhs,
  const expr_skeletont &full_lhs,
  const exprt &rhs,
  exprt::operandst &guard)
{
  if(is_ssa_expr(lhs))
  {
    assign_symbol(to_ssa_expr(lhs), full_lhs, rhs, guard);

    // Allocate shadow memory
    if(shadow_memory.has_value())
    {
      bool is_string_constant_init = is_string_constant_initialization(rhs);
      if(is_string_constant_init)
      {
        shadow_memory->symex_field_static_init_string_constant(
          state, to_ssa_expr(lhs), rhs);
      }
      else
      {
        shadow_memory->symex_field_static_init(state, to_ssa_expr(lhs));
      }
    }
  }
  else if(lhs.id() == ID_index)
    assign_array<use_update()>(to_index_expr(lhs), full_lhs, rhs, guard);
  else if(lhs.id()==ID_member)
  {
    const typet &type = to_member_expr(lhs).struct_op().type();
    if(type.id() == ID_struct || type.id() == ID_struct_tag)
    {
      assign_struct_member<use_update()>(
        to_member_expr(lhs), full_lhs, rhs, guard);
    }
    else if(type.id() == ID_union || type.id() == ID_union_tag)
    {
      // should have been replaced by byte_extract
      throw unsupported_operation_exceptiont(
        "assign_rec: unexpected assignment to union member");
    }
    else
      throw unsupported_operation_exceptiont(
        "assign_rec: unexpected assignment to member of '" + type.id_string() +
        "'");
  }
  else if(lhs.id()==ID_if)
    assign_if(to_if_expr(lhs), full_lhs, rhs, guard);
  else if(lhs.id()==ID_typecast)
    assign_typecast(to_typecast_expr(lhs), full_lhs, rhs, guard);
  else if(goto_symex_statet::is_read_only_object(lhs))
  {
    // ignore
  }
  else if(lhs.id()==ID_byte_extract_little_endian ||
          lhs.id()==ID_byte_extract_big_endian)
  {
    assign_byte_extract(to_byte_extract_expr(lhs), full_lhs, rhs, guard);
  }
  else if(lhs.id() == ID_complex_real)
  {
    // this is stuff like __real__ x = 1;
    const complex_real_exprt &complex_real_expr = to_complex_real_expr(lhs);

    const complex_imag_exprt complex_imag_expr(complex_real_expr.op());

    complex_exprt new_rhs(
      rhs, complex_imag_expr, to_complex_type(complex_real_expr.op().type()));

    assign_rec(complex_real_expr.op(), full_lhs, new_rhs, guard);
  }
  else if(lhs.id() == ID_complex_imag)
  {
    const complex_imag_exprt &complex_imag_expr = to_complex_imag_expr(lhs);

    const complex_real_exprt complex_real_expr(complex_imag_expr.op());

    complex_exprt new_rhs(
      complex_real_expr, rhs, to_complex_type(complex_imag_expr.op().type()));

    assign_rec(complex_imag_expr.op(), full_lhs, new_rhs, guard);
  }
  else
    throw unsupported_operation_exceptiont(
      "assignment to '" + lhs.id_string() + "' not handled");
}

/// Assignment from the rhs value to the lhs variable
struct assignmentt final
{
  ssa_exprt lhs;
  /// Skeleton to reconstruct the original lhs in the assignment
  expr_skeletont original_lhs_skeleton;
  exprt rhs;
};

/// Assign a struct expression to a symbol. If \ref symex_assignt::assign_symbol
/// was used then we would assign the whole symbol, before extracting its
/// components, with results like
/// `x = {1, 2}; x..field1 = x.field1; x..field2 = x.field2;`
/// This abbreviates the process, directly producing
/// `x..field1 = 1; x..field2 = 2;`
/// \param lhs: symbol to assign (already renamed to L1)
/// \param full_lhs: expression skeleton corresponding to \p lhs, to be included
///   in the result trace
/// \param rhs: struct expression to assign to \p lhs
/// \param guard: guard conjuncts that must hold for this assignment to be made
void symex_assignt::assign_from_struct(
  const ssa_exprt &lhs, // L1
  const expr_skeletont &full_lhs,
  const struct_exprt &rhs,
  const exprt::operandst &guard)
{
  const auto &components =
    lhs.type().id() == ID_struct_tag
      ? ns.follow_tag(to_struct_tag_type(lhs.type())).components()
      : to_struct_type(lhs.type()).components();
  PRECONDITION(rhs.operands().size() == components.size());

  for(const auto &comp_rhs : make_range(components).zip(rhs.operands()))
  {
    const auto &comp = comp_rhs.first;
    const exprt lhs_field = state.field_sensitivity.apply(
      ns, state, member_exprt{lhs, comp.get_name(), comp.type()}, true);
    INVARIANT(
      lhs_field.id() == ID_symbol,
      "member of symbol should be susceptible to field-sensitivity");

    assign_symbol(to_ssa_expr(lhs_field), full_lhs, comp_rhs.second, guard);
  }
}

void symex_assignt::assign_non_struct_symbol(
  const ssa_exprt &lhs, // L1
  const expr_skeletont &full_lhs,
  const exprt &rhs,
  const exprt::operandst &guard)
{
  exprt l2_rhs =
    state
      .rename(
        // put assignment guard into the rhs
        guard.empty()
          ? rhs
          : static_cast<exprt>(if_exprt{conjunction(guard), rhs, lhs}),
        ns)
      .get();

  assignmentt assignment{lhs, full_lhs, l2_rhs};

  if(symex_config.simplify_opt)
    assignment.rhs = simplify_expr(std::move(assignment.rhs), ns);

  const ssa_exprt l2_lhs = state
                             .assignment(
                               assignment.lhs,
                               assignment.rhs,
                               ns,
                               symex_config.simplify_opt,
                               symex_config.constant_propagation,
                               symex_config.allow_pointer_unsoundness)
                             .get();

  state.record_events.push(false);
  // Note any other symbols mentioned in the skeleton are rvalues -- for example
  // array indices -- and were renamed to L2 at the start of
  // goto_symext::symex_assign.
  const exprt l2_full_lhs = assignment.original_lhs_skeleton.apply(l2_lhs);
  if(symex_config.run_validation_checks)
  {
    INVARIANT(
      !check_renaming(l2_full_lhs), "l2_full_lhs should be renamed to L2");
  }
  state.record_events.pop();

  auto current_assignment_type =
    ns.lookup(l2_lhs.get_object_name()).is_auxiliary &&
        id2string(l2_lhs.get_object_name()).find(SHADOW_MEMORY_SYMBOL_PREFIX) !=
          std::string::npos
      ? symex_targett::assignment_typet::HIDDEN
      : assignment_type;

  target.assignment(
    make_and(state.guard.as_expr(), conjunction(guard)),
    l2_lhs,
    l2_full_lhs,
    get_original_name(l2_full_lhs),
    assignment.rhs,
    state.source,
    current_assignment_type);

  const ssa_exprt &l1_lhs = assignment.lhs;
  if(state.field_sensitivity.is_divisible(l1_lhs, false))
  {
    // Split composite symbol lhs into its components
    state.field_sensitivity.field_assignments(
      ns,
      state,
      l1_lhs,
      assignment.rhs,
      target,
      symex_config.allow_pointer_unsoundness);
  }
}

void symex_assignt::assign_symbol(
  const ssa_exprt &lhs, // L1
  const expr_skeletont &full_lhs,
  const exprt &rhs,
  const exprt::operandst &guard)
{
  // Shortcut the common case of a whole-struct initializer:
  if(rhs.id() == ID_struct)
    assign_from_struct(lhs, full_lhs, to_struct_expr(rhs), guard);
  else
    assign_non_struct_symbol(lhs, full_lhs, rhs, guard);
}

void symex_assignt::assign_typecast(
  const typecast_exprt &lhs,
  const expr_skeletont &full_lhs,
  const exprt &rhs,
  exprt::operandst &guard)
{
  // these may come from dereferencing on the lhs
  exprt rhs_typecasted = typecast_exprt::conditional_cast(rhs, lhs.op().type());
  expr_skeletont new_skeleton =
    full_lhs.compose(expr_skeletont::remove_op0(lhs));
  assign_rec(lhs.op(), new_skeleton, rhs_typecasted, guard);
}

template <bool use_update>
void symex_assignt::assign_array(
  const index_exprt &lhs,
  const expr_skeletont &full_lhs,
  const exprt &rhs,
  exprt::operandst &guard)
{
  const exprt &lhs_array=lhs.array();
  const exprt &lhs_index=lhs.index();
  const typet &lhs_index_type = lhs_array.type();

  PRECONDITION(lhs_index_type.id() == ID_array);

  if(use_update)
  {
    // turn
    //   a[i]=e
    // into
    //   a'==UPDATE(a, [i], e)
    const update_exprt new_rhs{lhs_array, index_designatort(lhs_index), rhs};
    const expr_skeletont new_skeleton =
      full_lhs.compose(expr_skeletont::remove_op0(lhs));
    assign_rec(lhs, new_skeleton, new_rhs, guard);
  }
  else
  {
    // turn
    //   a[i]=e
    // into
    //   a'==a WITH [i:=e]
    const with_exprt new_rhs{lhs_array, lhs_index, rhs};
    const expr_skeletont new_skeleton =
      full_lhs.compose(expr_skeletont::remove_op0(lhs));
    assign_rec(lhs_array, new_skeleton, new_rhs, guard);
  }
}

template <bool use_update>
void symex_assignt::assign_struct_member(
  const member_exprt &lhs,
  const expr_skeletont &full_lhs,
  const exprt &rhs,
  exprt::operandst &guard)
{
  // Symbolic execution of a struct member assignment.

  // lhs must be member operand, which
  // takes one operand, which must be a structure.

  exprt lhs_struct = lhs.op();

  // typecasts involved? C++ does that for inheritance.
  if(lhs_struct.id()==ID_typecast)
  {
    if(to_typecast_expr(lhs_struct).op().id() == ID_null_object)
    {
      // ignore, and give up
      return;
    }
    else
    {
      // remove the type cast, we assume that the member is there
      exprt tmp = to_typecast_expr(lhs_struct).op();

      if(tmp.type().id() == ID_struct || tmp.type().id() == ID_struct_tag)
        lhs_struct=tmp;
      else
        return; // ignore and give up
    }
  }

  const irep_idt &component_name=lhs.get_component_name();

  if(use_update)
  {
    // turn
    //   a.c=e
    // into
    //   a'==UPDATE(a, .c, e)
    const update_exprt new_rhs{
      lhs_struct, member_designatort(component_name), rhs};
    const expr_skeletont new_skeleton =
      full_lhs.compose(expr_skeletont::remove_op0(lhs));
    assign_rec(lhs_struct, new_skeleton, new_rhs, guard);
  }
  else
  {
    // turn
    //   a.c=e
    // into
    //   a'==a WITH [c:=e]

    with_exprt new_rhs(lhs_struct, exprt(ID_member_name), rhs);
    new_rhs.where().set(ID_component_name, component_name);
    const expr_skeletont new_skeleton =
      full_lhs.compose(expr_skeletont::remove_op0(lhs));
    assign_rec(lhs_struct, new_skeleton, new_rhs, guard);
  }
}

void symex_assignt::assign_if(
  const if_exprt &lhs,
  const expr_skeletont &full_lhs,
  const exprt &rhs,
  exprt::operandst &guard)
{
  // we have (c?a:b)=e;

  guard.push_back(lhs.cond());
  assign_rec(lhs.true_case(), full_lhs, rhs, guard);
  guard.pop_back();

  guard.push_back(not_exprt(lhs.cond()));
  assign_rec(lhs.false_case(), full_lhs, rhs, guard);
  guard.pop_back();
}

void symex_assignt::assign_byte_extract(
  const byte_extract_exprt &lhs,
  const expr_skeletont &full_lhs,
  const exprt &rhs,
  exprt::operandst &guard)
{
  // we have byte_extract_X(object, offset)=value
  // turn into object=byte_update_X(object, offset, value)

  irep_idt byte_update_id;
  if(lhs.id()==ID_byte_extract_little_endian)
    byte_update_id = ID_byte_update_little_endian;
  else if(lhs.id()==ID_byte_extract_big_endian)
    byte_update_id = ID_byte_update_big_endian;
  else
    UNREACHABLE;

  const byte_update_exprt new_rhs{
    byte_update_id, lhs.op(), lhs.offset(), rhs, lhs.get_bits_per_byte()};
  const expr_skeletont new_skeleton =
    full_lhs.compose(expr_skeletont::remove_op0(lhs));
  assign_rec(lhs.op(), new_skeleton, new_rhs, guard);
}
