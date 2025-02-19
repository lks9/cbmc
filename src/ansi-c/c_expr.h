/*******************************************************************\

Module: API to expression classes that are internal to the C frontend

Author: Daniel Kroening, kroening@kroening.com

\*******************************************************************/

#ifndef CPROVER_ANSI_C_C_EXPR_H
#define CPROVER_ANSI_C_C_EXPR_H

/// \file ansi-c/c_expr.h
/// API to expression classes that are internal to the C frontend

#include <util/byte_operators.h>
#include <util/std_code.h>

/// \brief Shuffle elements of one or two vectors, modelled after Clang's
/// __builtin_shufflevector.
class shuffle_vector_exprt : public multi_ary_exprt
{
public:
  shuffle_vector_exprt(
    exprt vector1,
    std::optional<exprt> vector2,
    exprt::operandst indices);

  const vector_typet &type() const
  {
    return static_cast<const vector_typet &>(multi_ary_exprt::type());
  }

  vector_typet &type()
  {
    return static_cast<vector_typet &>(multi_ary_exprt::type());
  }

  const exprt &vector1() const
  {
    return op0();
  }

  exprt &vector1()
  {
    return op0();
  }

  const exprt &vector2() const
  {
    return op1();
  }

  exprt &vector2()
  {
    return op1();
  }

  bool has_two_input_vectors() const
  {
    return vector2().is_not_nil();
  }

  const exprt::operandst &indices() const
  {
    return op2().operands();
  }

  exprt::operandst &indices()
  {
    return op2().operands();
  }

  vector_exprt lower() const;
};

template <>
inline bool can_cast_expr<shuffle_vector_exprt>(const exprt &base)
{
  return base.id() == ID_shuffle_vector;
}

inline void validate_expr(const shuffle_vector_exprt &value)
{
  validate_operands(value, 3, "shuffle_vector must have three operands");
}

/// \brief Cast an exprt to a \ref shuffle_vector_exprt
///
/// \a expr must be known to be \ref shuffle_vector_exprt.
///
/// \param expr: Source expression
/// \return Object of type \ref shuffle_vector_exprt
inline const shuffle_vector_exprt &to_shuffle_vector_expr(const exprt &expr)
{
  PRECONDITION(expr.id() == ID_shuffle_vector);
  const shuffle_vector_exprt &ret =
    static_cast<const shuffle_vector_exprt &>(expr);
  validate_expr(ret);
  return ret;
}

/// \copydoc to_shuffle_vector_expr(const exprt &)
inline shuffle_vector_exprt &to_shuffle_vector_expr(exprt &expr)
{
  PRECONDITION(expr.id() == ID_shuffle_vector);
  shuffle_vector_exprt &ret = static_cast<shuffle_vector_exprt &>(expr);
  validate_expr(ret);
  return ret;
}

/// \brief A Boolean expression returning true, iff the result of performing
/// operation \c kind on operands \c a and \c b in infinite-precision arithmetic
/// cannot be represented in the type of the object that \c result points to (or
/// the type of \c result, if it is not a pointer).
/// If \c result is a pointer, the result of the operation is stored in the
/// object pointed to by \c result.
class side_effect_expr_overflowt : public side_effect_exprt
{
public:
  side_effect_expr_overflowt(
    const irep_idt &kind,
    exprt _lhs,
    exprt _rhs,
    exprt _result,
    const source_locationt &loc)
    : side_effect_exprt(
        "overflow-" + id2string(kind),
        {std::move(_lhs), std::move(_rhs), std::move(_result)},
        bool_typet{},
        loc)
  {
  }

  exprt &lhs()
  {
    return op0();
  }

  const exprt &lhs() const
  {
    return op0();
  }

  exprt &rhs()
  {
    return op1();
  }

  const exprt &rhs() const
  {
    return op1();
  }

  exprt &result()
  {
    return op2();
  }

  const exprt &result() const
  {
    return op2();
  }
};

template <>
inline bool can_cast_expr<side_effect_expr_overflowt>(const exprt &base)
{
  if(base.id() != ID_side_effect)
    return false;

  const irep_idt &statement = to_side_effect_expr(base).get_statement();
  return statement == ID_overflow_plus || statement == ID_overflow_mult ||
         statement == ID_overflow_minus;
}

/// \brief Cast an exprt to a \ref side_effect_expr_overflowt
///
/// \a expr must be known to be \ref side_effect_expr_overflowt.
///
/// \param expr: Source expression
/// \return Object of type \ref side_effect_expr_overflowt
inline const side_effect_expr_overflowt &
to_side_effect_expr_overflow(const exprt &expr)
{
  const auto &side_effect_expr = to_side_effect_expr(expr);
  PRECONDITION(
    side_effect_expr.get_statement() == ID_overflow_plus ||
    side_effect_expr.get_statement() == ID_overflow_mult ||
    side_effect_expr.get_statement() == ID_overflow_minus);
  return static_cast<const side_effect_expr_overflowt &>(side_effect_expr);
}

/// \copydoc to_side_effect_expr_overflow(const exprt &)
inline side_effect_expr_overflowt &to_side_effect_expr_overflow(exprt &expr)
{
  auto &side_effect_expr = to_side_effect_expr(expr);
  PRECONDITION(
    side_effect_expr.get_statement() == ID_overflow_plus ||
    side_effect_expr.get_statement() == ID_overflow_mult ||
    side_effect_expr.get_statement() == ID_overflow_minus);
  return static_cast<side_effect_expr_overflowt &>(side_effect_expr);
}

/// \brief A class for an expression that indicates a history variable
class history_exprt : public unary_exprt
{
public:
  explicit history_exprt(exprt variable, const irep_idt &id)
    : unary_exprt(id, std::move(variable))
  {
  }

  const exprt &expression() const
  {
    return op0();
  }
};

inline const history_exprt &
to_history_expr(const exprt &expr, const irep_idt &id)
{
  PRECONDITION(id == ID_old || id == ID_loop_entry);
  PRECONDITION(expr.id() == id);
  auto &ret = static_cast<const history_exprt &>(expr);
  validate_expr(ret);
  return ret;
}

/// \brief A class for an expression that represents a conditional target or
/// a list of targets sharing a common condition
/// in an assigns clause.
class conditional_target_group_exprt : public exprt
{
public:
  explicit conditional_target_group_exprt(exprt _condition, exprt _target_list)
    : exprt(ID_conditional_target_group, empty_typet{})
  {
    add_to_operands(std::move(_condition));
    add_to_operands(std::move(_target_list));
  }

  static void check(
    const exprt &expr,
    const validation_modet vm = validation_modet::INVARIANT)
  {
    DATA_CHECK(
      vm,
      expr.operands().size() == 2,
      "conditional target expression must have two operands");

    DATA_CHECK(
      vm,
      expr.operands()[1].id() == ID_expression_list,
      "conditional target second operand must be an ID_expression_list "
      "expression, found " +
        id2string(expr.operands()[1].id()));
  }

  static void validate(
    const exprt &expr,
    const namespacet &,
    const validation_modet vm = validation_modet::INVARIANT)
  {
    check(expr, vm);
  }

  const exprt &condition() const
  {
    return op0();
  }

  exprt &condition()
  {
    return op0();
  }

  const exprt::operandst &targets() const
  {
    return op1().operands();
  }

  exprt::operandst &targets()
  {
    return op1().operands();
  }
};

inline void validate_expr(const conditional_target_group_exprt &value)
{
  conditional_target_group_exprt::check(value);
}

template <>
inline bool can_cast_expr<conditional_target_group_exprt>(const exprt &base)
{
  return base.id() == ID_conditional_target_group;
}

/// \brief Cast an exprt to a \ref conditional_target_group_exprt
///
/// \a expr must be known to be \ref conditional_target_group_exprt
///
/// \param expr: Source expression
/// \return Object of type \ref conditional_target_group_exprt
inline const conditional_target_group_exprt &
to_conditional_target_group_expr(const exprt &expr)
{
  PRECONDITION(expr.id() == ID_conditional_target_group);
  auto &ret = static_cast<const conditional_target_group_exprt &>(expr);
  validate_expr(ret);
  return ret;
}

/// \copydoc to_conditional_target_group_expr(const exprt &expr)
inline conditional_target_group_exprt &
to_conditional_target_group_expr(exprt &expr)
{
  PRECONDITION(expr.id() == ID_conditional_target_group);
  auto &ret = static_cast<conditional_target_group_exprt &>(expr);
  validate_expr(ret);
  return ret;
}

/// \brief A Boolean expression returning true, iff the value of an enum-typed
/// symbol equals one of the enum's declared values.
class enum_is_in_range_exprt : public unary_predicate_exprt
{
public:
  explicit enum_is_in_range_exprt(exprt _op)
    : unary_predicate_exprt(ID_enum_is_in_range, std::move(_op))
  {
    op().add_source_location().add_pragma("disable:enum-range-check");
  }

  exprt lower(const namespacet &ns) const;
};

template <>
inline bool can_cast_expr<enum_is_in_range_exprt>(const exprt &base)
{
  return base.id() == ID_enum_is_in_range;
}

inline void validate_expr(const enum_is_in_range_exprt &expr)
{
  validate_operands(
    expr, 1, "enum_is_in_range expression must have one operand");
}

/// \brief Cast an exprt to an \ref enum_is_in_range_exprt
///
/// \a expr must be known to be \ref enum_is_in_range_exprt.
///
/// \param expr: Source expression
/// \return Object of type \ref enum_is_in_range_exprt
inline const enum_is_in_range_exprt &to_enum_is_in_range_expr(const exprt &expr)
{
  PRECONDITION(expr.id() == ID_enum_is_in_range);
  const enum_is_in_range_exprt &ret =
    static_cast<const enum_is_in_range_exprt &>(expr);
  validate_expr(ret);
  return ret;
}

/// \copydoc to_side_effect_expr_overflow(const exprt &)
inline enum_is_in_range_exprt &to_enum_is_in_range_expr(exprt &expr)
{
  PRECONDITION(expr.id() == ID_enum_is_in_range);
  enum_is_in_range_exprt &ret = static_cast<enum_is_in_range_exprt &>(expr);
  validate_expr(ret);
  return ret;
}

/// \brief Reinterpret the bits of an expression of type `S` as an expression of
/// type `T` where `S` and `T` have the same size.
class bit_cast_exprt : public unary_exprt
{
public:
  bit_cast_exprt(exprt expr, typet type)
    : unary_exprt(ID_bit_cast, std::move(expr), std::move(type))
  {
  }

  byte_extract_exprt lower() const;
};

template <>
inline bool can_cast_expr<bit_cast_exprt>(const exprt &base)
{
  return base.id() == ID_bit_cast;
}

inline void validate_expr(const bit_cast_exprt &value)
{
  validate_operands(value, 1, "bit_cast must have one operand");
}

/// \brief Cast an exprt to a \ref bit_cast_exprt
///
/// \a expr must be known to be \ref bit_cast_exprt.
///
/// \param expr: Source expression
/// \return Object of type \ref bit_cast_exprt
inline const bit_cast_exprt &to_bit_cast_expr(const exprt &expr)
{
  PRECONDITION(expr.id() == ID_bit_cast);
  const bit_cast_exprt &ret = static_cast<const bit_cast_exprt &>(expr);
  validate_expr(ret);
  return ret;
}

/// \copydoc to_bit_cast_expr(const exprt &)
inline bit_cast_exprt &to_bit_cast_expr(exprt &expr)
{
  PRECONDITION(expr.id() == ID_bit_cast);
  bit_cast_exprt &ret = static_cast<bit_cast_exprt &>(expr);
  validate_expr(ret);
  return ret;
}

#endif // CPROVER_ANSI_C_C_EXPR_H
