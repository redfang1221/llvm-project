//===- ExpandPatterns.cpp - Code to expand various math operations. -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements expansion of various math operations.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Dialect/Math/Transforms/Passes.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Transforms/DialectConversion.h"

using namespace mlir;

/// Create a float constant.
static Value createFloatConst(Location loc, Type type, APFloat value,
                              OpBuilder &b) {
  bool losesInfo = false;
  auto eltType = getElementTypeOrSelf(type);
  // Convert double to the given `FloatType` with round-to-nearest-ties-to-even.
  value.convert(cast<FloatType>(eltType).getFloatSemantics(),
                APFloat::rmNearestTiesToEven, &losesInfo);
  auto attr = b.getFloatAttr(eltType, value);
  if (auto shapedTy = dyn_cast<ShapedType>(type)) {
    return b.create<arith::ConstantOp>(loc,
                                       DenseElementsAttr::get(shapedTy, attr));
  }

  return b.create<arith::ConstantOp>(loc, attr);
}

static Value createFloatConst(Location loc, Type type, double value,
                              OpBuilder &b) {
  return createFloatConst(loc, type, APFloat(value), b);
}

/// Create an integer constant.
static Value createIntConst(Location loc, Type type, int64_t value,
                            OpBuilder &b) {
  auto attr = b.getIntegerAttr(getElementTypeOrSelf(type), value);
  if (auto shapedTy = dyn_cast<ShapedType>(type)) {
    return b.create<arith::ConstantOp>(loc,
                                       DenseElementsAttr::get(shapedTy, attr));
  }

  return b.create<arith::ConstantOp>(loc, attr);
}

static Value createTruncatedFPValue(Value operand, ImplicitLocOpBuilder &b) {
  Type opType = operand.getType();
  Type i64Ty = b.getI64Type();
  if (auto shapedTy = dyn_cast<ShapedType>(opType))
    i64Ty = shapedTy.clone(i64Ty);
  Value fixedConvert = b.create<arith::FPToSIOp>(i64Ty, operand);
  Value fpFixedConvert = b.create<arith::SIToFPOp>(opType, fixedConvert);
  // The truncation does not preserve the sign when the truncated
  // value is -0. So here the sign is copied again.
  return b.create<math::CopySignOp>(fpFixedConvert, operand);
}

// sinhf(float x) -> (exp(x) - exp(-x)) / 2
static LogicalResult convertSinhOp(math::SinhOp op, PatternRewriter &rewriter) {
  ImplicitLocOpBuilder b(op->getLoc(), rewriter);
  Value operand = op.getOperand();
  Type opType = operand.getType();

  Value exp = b.create<math::ExpOp>(operand);
  Value neg = b.create<arith::NegFOp>(operand);
  Value nexp = b.create<math::ExpOp>(neg);
  Value sub = b.create<arith::SubFOp>(exp, nexp);
  Value half = createFloatConst(op->getLoc(), opType, 0.5, rewriter);
  Value res = b.create<arith::MulFOp>(sub, half);
  rewriter.replaceOp(op, res);
  return success();
}

// coshf(float x) -> (exp(x) + exp(-x)) / 2
static LogicalResult convertCoshOp(math::CoshOp op, PatternRewriter &rewriter) {
  ImplicitLocOpBuilder b(op->getLoc(), rewriter);
  Value operand = op.getOperand();
  Type opType = operand.getType();

  Value exp = b.create<math::ExpOp>(operand);
  Value neg = b.create<arith::NegFOp>(operand);
  Value nexp = b.create<math::ExpOp>(neg);
  Value add = b.create<arith::AddFOp>(exp, nexp);
  Value half = createFloatConst(op->getLoc(), opType, 0.5, rewriter);
  Value res = b.create<arith::MulFOp>(add, half);
  rewriter.replaceOp(op, res);
  return success();
}

/// Expands tanh op into
/// 1-exp^{-2x} / 1+exp^{-2x}
/// To avoid overflow we exploit the reflection symmetry `tanh(-x) = -tanh(x)`.
/// We compute a "signs" value which is -1 if input is negative and +1 if input
/// is positive.  Then multiply the input by this value, guaranteeing that the
/// result is positive, which also guarantees `exp^{-2x * sign(x)}` is in (0,
/// 1]. Expand the computation on the input `x * sign(x)`, then multiply the
/// result by `sign(x)` to retain sign of the real result.
static LogicalResult convertTanhOp(math::TanhOp op, PatternRewriter &rewriter) {
  auto floatType = op.getOperand().getType();
  Location loc = op.getLoc();
  Value zero = createFloatConst(loc, floatType, 0.0, rewriter);
  Value one = createFloatConst(loc, floatType, 1.0, rewriter);
  Value negTwo = createFloatConst(loc, floatType, -2.0, rewriter);

  // Compute sign(x) = cast<float_type>(x < 0) * (-2) + 1
  Value isNegative = rewriter.create<arith::CmpFOp>(
      loc, arith::CmpFPredicate::OLT, op.getOperand(), zero);
  Value isNegativeFloat =
      rewriter.create<arith::UIToFPOp>(loc, floatType, isNegative);
  Value isNegativeTimesNegTwo =
      rewriter.create<arith::MulFOp>(loc, isNegativeFloat, negTwo);
  Value sign = rewriter.create<arith::AddFOp>(loc, isNegativeTimesNegTwo, one);

  // Normalize input to positive value: y = sign(x) * x
  Value positiveX = rewriter.create<arith::MulFOp>(loc, sign, op.getOperand());

  // Decompose on normalized input
  Value negDoubledX = rewriter.create<arith::MulFOp>(loc, negTwo, positiveX);
  Value exp2x = rewriter.create<math::ExpOp>(loc, negDoubledX);
  Value dividend = rewriter.create<arith::SubFOp>(loc, one, exp2x);
  Value divisor = rewriter.create<arith::AddFOp>(loc, one, exp2x);
  Value positiveRes = rewriter.create<arith::DivFOp>(loc, dividend, divisor);

  // Multiply result by sign(x) to retain signs from negative inputs
  rewriter.replaceOpWithNewOp<arith::MulFOp>(op, sign, positiveRes);

  return success();
}

// Converts math.tan to math.sin, math.cos, and arith.divf.
static LogicalResult convertTanOp(math::TanOp op, PatternRewriter &rewriter) {
  ImplicitLocOpBuilder b(op->getLoc(), rewriter);
  Value operand = op.getOperand();
  Type type = operand.getType();
  Value sin = b.create<math::SinOp>(type, operand);
  Value cos = b.create<math::CosOp>(type, operand);
  Value div = b.create<arith::DivFOp>(type, sin, cos);
  rewriter.replaceOp(op, div);
  return success();
}

// asinh(float x) -> log(x + sqrt(x**2 + 1))
static LogicalResult convertAsinhOp(math::AsinhOp op,
                                    PatternRewriter &rewriter) {
  ImplicitLocOpBuilder b(op->getLoc(), rewriter);
  Value operand = op.getOperand();
  Type opType = operand.getType();

  Value one = createFloatConst(op->getLoc(), opType, 1.0, rewriter);
  Value fma = b.create<math::FmaOp>(operand, operand, one);
  Value sqrt = b.create<math::SqrtOp>(fma);
  Value add = b.create<arith::AddFOp>(operand, sqrt);
  Value res = b.create<math::LogOp>(add);
  rewriter.replaceOp(op, res);
  return success();
}

// acosh(float x) -> log(x + sqrt(x**2 - 1))
static LogicalResult convertAcoshOp(math::AcoshOp op,
                                    PatternRewriter &rewriter) {
  ImplicitLocOpBuilder b(op->getLoc(), rewriter);
  Value operand = op.getOperand();
  Type opType = operand.getType();

  Value negOne = createFloatConst(op->getLoc(), opType, -1.0, rewriter);
  Value fma = b.create<math::FmaOp>(operand, operand, negOne);
  Value sqrt = b.create<math::SqrtOp>(fma);
  Value add = b.create<arith::AddFOp>(operand, sqrt);
  Value res = b.create<math::LogOp>(add);
  rewriter.replaceOp(op, res);
  return success();
}

// atanh(float x) -> log((1 + x) / (1 - x)) / 2
static LogicalResult convertAtanhOp(math::AtanhOp op,
                                    PatternRewriter &rewriter) {
  ImplicitLocOpBuilder b(op->getLoc(), rewriter);
  Value operand = op.getOperand();
  Type opType = operand.getType();

  Value one = createFloatConst(op->getLoc(), opType, 1.0, rewriter);
  Value add = b.create<arith::AddFOp>(operand, one);
  Value neg = b.create<arith::NegFOp>(operand);
  Value sub = b.create<arith::AddFOp>(neg, one);
  Value div = b.create<arith::DivFOp>(add, sub);
  Value log = b.create<math::LogOp>(div);
  Value half = createFloatConst(op->getLoc(), opType, 0.5, rewriter);
  Value res = b.create<arith::MulFOp>(log, half);
  rewriter.replaceOp(op, res);
  return success();
}

static LogicalResult convertFmaFOp(math::FmaOp op, PatternRewriter &rewriter) {
  ImplicitLocOpBuilder b(op->getLoc(), rewriter);
  Value operandA = op.getOperand(0);
  Value operandB = op.getOperand(1);
  Value operandC = op.getOperand(2);
  Type type = op.getType();
  Value mult = b.create<arith::MulFOp>(type, operandA, operandB);
  Value add = b.create<arith::AddFOp>(type, mult, operandC);
  rewriter.replaceOp(op, add);
  return success();
}

// Converts a ceilf() function to the following:
// ceilf(float x) ->
//      y = (float)(int) x
//      if (x > y) then incr = 1 else incr = 0
//      y = y + incr   <= replace this op with the ceilf op.
static LogicalResult convertCeilOp(math::CeilOp op, PatternRewriter &rewriter) {
  // Creating constants assumes the static shaped type.
  auto shapedType = dyn_cast<ShapedType>(op.getType());
  if (shapedType && !shapedType.hasStaticShape())
    return failure();

  ImplicitLocOpBuilder b(op->getLoc(), rewriter);
  Value operand = op.getOperand();
  Type opType = operand.getType();
  Value fpFixedConvert = createTruncatedFPValue(operand, b);

  // Creating constants for later use.
  Value zero = createFloatConst(op->getLoc(), opType, 0.00, rewriter);
  Value one = createFloatConst(op->getLoc(), opType, 1.00, rewriter);

  Value gtCheck = b.create<arith::CmpFOp>(arith::CmpFPredicate::OGT, operand,
                                          fpFixedConvert);
  Value incrValue = b.create<arith::SelectOp>(op->getLoc(), gtCheck, one, zero);

  Value ret = b.create<arith::AddFOp>(opType, fpFixedConvert, incrValue);
  rewriter.replaceOp(op, ret);
  return success();
}

// Convert `math.fpowi` to a series of `arith.mulf` operations.
// If the power is negative, we divide one by the result.
// If both the base and power are zero, the result is 1.
// In the case of non constant power, we convert the operation to `math.powf`.
static LogicalResult convertFPowIOp(math::FPowIOp op,
                                    PatternRewriter &rewriter) {
  ImplicitLocOpBuilder b(op->getLoc(), rewriter);
  Value base = op.getOperand(0);
  Value power = op.getOperand(1);
  Type baseType = base.getType();

  auto convertFPowItoPowf = [&]() -> LogicalResult {
    Value castPowerToFp =
        rewriter.create<arith::SIToFPOp>(op.getLoc(), baseType, power);
    Value res = rewriter.create<math::PowFOp>(op.getLoc(), baseType, base,
                                              castPowerToFp);
    rewriter.replaceOp(op, res);
    return success();
  };

  Attribute cstAttr;
  if (!matchPattern(power, m_Constant(&cstAttr)))
    return convertFPowItoPowf();

  APInt value;
  if (!matchPattern(cstAttr, m_ConstantInt(&value)))
    return convertFPowItoPowf();

  int64_t powerInt = value.getSExtValue();
  bool isNegative = powerInt < 0;
  int64_t absPower = std::abs(powerInt);
  Value one = createFloatConst(op->getLoc(), baseType, 1.00, rewriter);
  Value res = createFloatConst(op->getLoc(), baseType, 1.00, rewriter);

  while (absPower > 0) {
    if (absPower & 1)
      res = b.create<arith::MulFOp>(baseType, base, res);
    absPower >>= 1;
    base = b.create<arith::MulFOp>(baseType, base, base);
  }

  // Make sure not to introduce UB in case of negative power.
  if (isNegative) {
    auto &sem = dyn_cast<mlir::FloatType>(getElementTypeOrSelf(baseType))
                    .getFloatSemantics();
    Value zero =
        createFloatConst(op->getLoc(), baseType,
                         APFloat::getZero(sem, /*Negative=*/false), rewriter);
    Value negZero =
        createFloatConst(op->getLoc(), baseType,
                         APFloat::getZero(sem, /*Negative=*/true), rewriter);
    Value posInfinity =
        createFloatConst(op->getLoc(), baseType,
                         APFloat::getInf(sem, /*Negative=*/false), rewriter);
    Value negInfinity =
        createFloatConst(op->getLoc(), baseType,
                         APFloat::getInf(sem, /*Negative=*/true), rewriter);
    Value zeroEqCheck =
        b.create<arith::CmpFOp>(arith::CmpFPredicate::OEQ, res, zero);
    Value negZeroEqCheck =
        b.create<arith::CmpFOp>(arith::CmpFPredicate::OEQ, res, negZero);
    res = b.create<arith::DivFOp>(baseType, one, res);
    res =
        b.create<arith::SelectOp>(op->getLoc(), zeroEqCheck, posInfinity, res);
    res = b.create<arith::SelectOp>(op->getLoc(), negZeroEqCheck, negInfinity,
                                    res);
  }

  rewriter.replaceOp(op, res);
  return success();
}

// Converts Powf(float a, float b) (meaning a^b) to exp^(b * ln(a))
// Some special cases where b is constant are handled separately:
// when b == 0, or |b| == 0.5, 1.0, or 2.0.
static LogicalResult convertPowfOp(math::PowFOp op, PatternRewriter &rewriter) {
  ImplicitLocOpBuilder b(op->getLoc(), rewriter);
  Value operandA = op.getOperand(0);
  Value operandB = op.getOperand(1);
  auto typeA = operandA.getType();
  auto typeB = operandB.getType();

  auto &sem =
      cast<mlir::FloatType>(getElementTypeOrSelf(typeB)).getFloatSemantics();
  APFloat valueB(sem);
  auto mulf = [&](Value x, Value y) -> Value {
    return b.create<arith::MulFOp>(x, y);
  };
  if (matchPattern(operandB, m_ConstantFloat(&valueB))) {
    if (valueB.isZero()) {
      // a^0 -> 1
      Value one = createFloatConst(op->getLoc(), typeA, 1.0, rewriter);
      rewriter.replaceOp(op, one);
      return success();
    }
    if (valueB.isExactlyValue(1.0)) {
      // a^1 -> a
      rewriter.replaceOp(op, operandA);
      return success();
    }
    if (valueB.isExactlyValue(-1.0)) {
      // a^(-1) -> 1 / a
      Value one = createFloatConst(op->getLoc(), typeA, 1.0, rewriter);
      Value div = b.create<arith::DivFOp>(one, operandA);
      rewriter.replaceOp(op, div);
      return success();
    }
    if (valueB.isExactlyValue(0.5)) {
      // a^(1/2) -> sqrt(a)
      Value sqrt = b.create<math::SqrtOp>(operandA);
      rewriter.replaceOp(op, sqrt);
      return success();
    }
    if (valueB.isExactlyValue(-0.5)) {
      // a^(-1/2) -> 1 / sqrt(a)
      Value rsqrt = b.create<math::RsqrtOp>(operandA);
      rewriter.replaceOp(op, rsqrt);
      return success();
    }
    if (valueB.isExactlyValue(2.0)) {
      // a^2 -> a * a
      rewriter.replaceOp(op, mulf(operandA, operandA));
      return success();
    }
    if (valueB.isExactlyValue(-2.0)) {
      // a^(-2) -> 1 / (a * a)
      Value one =
          createFloatConst(op->getLoc(), operandA.getType(), 1.0, rewriter);
      Value div = b.create<arith::DivFOp>(one, mulf(operandA, operandA));
      rewriter.replaceOp(op, div);
      return success();
    }
    if (valueB.isExactlyValue(3.0)) {
      rewriter.replaceOp(op, mulf(mulf(operandA, operandA), operandA));
      return success();
    }
  }

  Value logA = b.create<math::LogOp>(operandA);
  Value mult = b.create<arith::MulFOp>(operandB, logA);
  Value expResult = b.create<math::ExpOp>(mult);
  rewriter.replaceOp(op, expResult);
  return success();
}

// exp2f(float x) -> exp(x * ln(2))
//   Proof: Let's say 2^x = y
//   ln(2^x) = ln(y)
//   x * ln(2) = ln(y) => e ^(x*ln(2)) = y
static LogicalResult convertExp2fOp(math::Exp2Op op,
                                    PatternRewriter &rewriter) {
  ImplicitLocOpBuilder b(op->getLoc(), rewriter);
  Value operand = op.getOperand();
  Type opType = operand.getType();
  Value ln2 = createFloatConst(op->getLoc(), opType, llvm::numbers::ln2, b);
  Value mult = b.create<arith::MulFOp>(opType, operand, ln2);
  Value exp = b.create<math::ExpOp>(op->getLoc(), mult);
  rewriter.replaceOp(op, exp);
  return success();
}

static LogicalResult convertRoundOp(math::RoundOp op,
                                    PatternRewriter &rewriter) {
  Location loc = op.getLoc();
  ImplicitLocOpBuilder b(loc, rewriter);
  Value operand = op.getOperand();
  Type opType = operand.getType();
  Type opEType = getElementTypeOrSelf(opType);

  if (!opEType.isF32()) {
    return rewriter.notifyMatchFailure(op, "not a round of f32.");
  }

  Type i32Ty = b.getI32Type();
  if (auto shapedTy = dyn_cast<ShapedType>(opType))
    i32Ty = shapedTy.clone(i32Ty);

  Value half = createFloatConst(loc, opType, 0.5, b);
  Value c23 = createIntConst(loc, i32Ty, 23, b);
  Value c127 = createIntConst(loc, i32Ty, 127, b);
  Value expMask = createIntConst(loc, i32Ty, (1 << 8) - 1, b);

  Value incrValue = b.create<math::CopySignOp>(half, operand);
  Value add = b.create<arith::AddFOp>(opType, operand, incrValue);
  Value fpFixedConvert = createTruncatedFPValue(add, b);

  // There are three cases where adding 0.5 to the value and truncating by
  // converting to an i64 does not result in the correct behavior:
  //
  // 1. Special values: +-inf and +-nan
  //     Casting these special values to i64 has undefined behavior. To identify
  //     these values, we use the fact that these values are the only float
  //     values with the maximum possible biased exponent.
  //
  // 2. Large values: 2^23 <= |x| <= INT_64_MAX
  //     Adding 0.5 to a float larger than or equal to 2^23 results in precision
  //     errors that sometimes round the value up and sometimes round the value
  //     down. For example:
  //         8388608.0 + 0.5 = 8388608.0
  //         8388609.0 + 0.5 = 8388610.0
  //
  // 3. Very large values: |x| > INT_64_MAX
  //     Casting to i64 a value greater than the max i64 value will overflow the
  //     i64 leading to wrong outputs.
  //
  // All three cases satisfy the property `biasedExp >= 23`.
  Value operandBitcast = b.create<arith::BitcastOp>(i32Ty, operand);
  Value operandExp = b.create<arith::AndIOp>(
      b.create<arith::ShRUIOp>(operandBitcast, c23), expMask);
  Value operandBiasedExp = b.create<arith::SubIOp>(operandExp, c127);
  Value isSpecialValOrLargeVal =
      b.create<arith::CmpIOp>(arith::CmpIPredicate::sge, operandBiasedExp, c23);

  Value result = b.create<arith::SelectOp>(isSpecialValOrLargeVal, operand,
                                           fpFixedConvert);
  rewriter.replaceOp(op, result);
  return success();
}

// Converts math.ctlz to scf and arith operations. This is done
// by performing a binary search on the bits.
static LogicalResult convertCtlzOp(math::CountLeadingZerosOp op,
                                   PatternRewriter &rewriter) {
  auto operand = op.getOperand();
  auto operandTy = operand.getType();
  auto eTy = getElementTypeOrSelf(operandTy);
  Location loc = op.getLoc();

  int32_t bitwidth = eTy.getIntOrFloatBitWidth();
  if (bitwidth > 64)
    return failure();

  uint64_t allbits = -1;
  if (bitwidth < 64) {
    allbits = allbits >> (64 - bitwidth);
  }

  Value x = operand;
  Value count = createIntConst(loc, operandTy, 0, rewriter);
  for (int32_t bw = bitwidth; bw > 1; bw = bw / 2) {
    auto half = bw / 2;
    auto bits = createIntConst(loc, operandTy, half, rewriter);
    auto mask = createIntConst(loc, operandTy, allbits >> half, rewriter);

    Value pred =
        rewriter.create<arith::CmpIOp>(loc, arith::CmpIPredicate::ule, x, mask);
    Value add = rewriter.create<arith::AddIOp>(loc, count, bits);
    Value shift = rewriter.create<arith::ShLIOp>(loc, x, bits);

    x = rewriter.create<arith::SelectOp>(loc, pred, shift, x);
    count = rewriter.create<arith::SelectOp>(loc, pred, add, count);
  }

  Value zero = createIntConst(loc, operandTy, 0, rewriter);
  Value pred = rewriter.create<arith::CmpIOp>(loc, arith::CmpIPredicate::eq,
                                              operand, zero);

  Value bwval = createIntConst(loc, operandTy, bitwidth, rewriter);
  Value sel = rewriter.create<arith::SelectOp>(loc, pred, bwval, count);
  rewriter.replaceOp(op, sel);
  return success();
}

// Convert `math.roundeven` into `math.round` + arith ops
static LogicalResult convertRoundEvenOp(math::RoundEvenOp op,
                                        PatternRewriter &rewriter) {
  Location loc = op.getLoc();
  ImplicitLocOpBuilder b(loc, rewriter);
  auto operand = op.getOperand();
  Type operandTy = operand.getType();
  Type resultTy = op.getType();
  Type operandETy = getElementTypeOrSelf(operandTy);
  Type resultETy = getElementTypeOrSelf(resultTy);

  if (!isa<FloatType>(operandETy) || !isa<FloatType>(resultETy)) {
    return rewriter.notifyMatchFailure(op, "not a roundeven of f16 or f32.");
  }

  Type fTy = operandTy;
  Type iTy = rewriter.getIntegerType(operandETy.getIntOrFloatBitWidth());
  if (auto shapedTy = dyn_cast<ShapedType>(fTy)) {
    iTy = shapedTy.clone(iTy);
  }

  unsigned bitWidth = operandETy.getIntOrFloatBitWidth();
  // The width returned by getFPMantissaWidth includes the integer bit.
  unsigned mantissaWidth =
      llvm::cast<FloatType>(operandETy).getFPMantissaWidth() - 1;
  unsigned exponentWidth = bitWidth - mantissaWidth - 1;

  // The names of the variables correspond to f32.
  // f64: 1 bit sign | 11 bits exponent | 52 bits mantissa.
  // f32: 1 bit sign | 8 bits exponent  | 23 bits mantissa.
  // f16: 1 bit sign | 5 bits exponent  | 10 bits mantissa.
  Value c1Float = createFloatConst(loc, fTy, 1.0, b);
  Value c0 = createIntConst(loc, iTy, 0, b);
  Value c1 = createIntConst(loc, iTy, 1, b);
  Value cNeg1 = createIntConst(loc, iTy, -1, b);
  Value c23 = createIntConst(loc, iTy, mantissaWidth, b);
  Value c31 = createIntConst(loc, iTy, bitWidth - 1, b);
  Value c127 = createIntConst(loc, iTy, (1ull << (exponentWidth - 1)) - 1, b);
  Value c2To22 = createIntConst(loc, iTy, 1ull << (mantissaWidth - 1), b);
  Value c23Mask = createIntConst(loc, iTy, (1ull << mantissaWidth) - 1, b);
  Value expMask = createIntConst(loc, iTy, (1ull << exponentWidth) - 1, b);

  Value operandBitcast = b.create<arith::BitcastOp>(iTy, operand);
  Value round = b.create<math::RoundOp>(operand);
  Value roundBitcast = b.create<arith::BitcastOp>(iTy, round);

  // Get biased exponents for operand and round(operand)
  Value operandExp = b.create<arith::AndIOp>(
      b.create<arith::ShRUIOp>(operandBitcast, c23), expMask);
  Value operandBiasedExp = b.create<arith::SubIOp>(operandExp, c127);
  Value roundExp = b.create<arith::AndIOp>(
      b.create<arith::ShRUIOp>(roundBitcast, c23), expMask);
  Value roundBiasedExp = b.create<arith::SubIOp>(roundExp, c127);

  auto safeShiftRight = [&](Value x, Value shift) -> Value {
    // Clamp shift to valid range [0, bitwidth - 1] to avoid undefined behavior
    Value clampedShift = b.create<arith::MaxSIOp>(shift, c0);
    clampedShift = b.create<arith::MinSIOp>(clampedShift, c31);
    return b.create<arith::ShRUIOp>(x, clampedShift);
  };

  auto maskMantissa = [&](Value mantissa,
                          Value mantissaMaskRightShift) -> Value {
    Value shiftedMantissaMask = safeShiftRight(c23Mask, mantissaMaskRightShift);
    return b.create<arith::AndIOp>(mantissa, shiftedMantissaMask);
  };

  // A whole number `x`, such that `|x| != 1`, is even if the mantissa, ignoring
  // the leftmost `clamp(biasedExp - 1, 0, 23)` bits, is zero. Large numbers
  // with `biasedExp > 23` (numbers where there is not enough precision to store
  // decimals) are always even, and they satisfy the even condition trivially
  // since the mantissa without all its bits is zero. The even condition
  // is also true for +-0, since they have `biasedExp = -127` and the entire
  // mantissa is zero. The case of +-1 has to be handled separately. Here
  // we identify these values by noting that +-1 are the only whole numbers with
  // `biasedExp == 0`.
  //
  // The special values +-inf and +-nan also satisfy the same property that
  // whole non-unit even numbers satisfy. In particular, the special values have
  // `biasedExp > 23`, so they get treated as large numbers with no room for
  // decimals, which are always even.
  Value roundBiasedExpEq0 =
      b.create<arith::CmpIOp>(arith::CmpIPredicate::eq, roundBiasedExp, c0);
  Value roundBiasedExpMinus1 = b.create<arith::SubIOp>(roundBiasedExp, c1);
  Value roundMaskedMantissa = maskMantissa(roundBitcast, roundBiasedExpMinus1);
  Value roundIsNotEvenOrSpecialVal = b.create<arith::CmpIOp>(
      arith::CmpIPredicate::ne, roundMaskedMantissa, c0);
  roundIsNotEvenOrSpecialVal =
      b.create<arith::OrIOp>(roundIsNotEvenOrSpecialVal, roundBiasedExpEq0);

  // A value `x` with `0 <= biasedExp < 23`, is halfway between two consecutive
  // integers if the bit at index `biasedExp` starting from the left in the
  // mantissa is 1 and all the bits to the right are zero. Values with
  // `biasedExp >= 23` don't have decimals, so they are never halfway. The
  // values +-0.5 are the only halfway values that have `biasedExp == -1 < 0`,
  // so these are handled separately. In particular, if `biasedExp == -1`, the
  // value is halfway if the entire mantissa is zero.
  Value operandBiasedExpEqNeg1 = b.create<arith::CmpIOp>(
      arith::CmpIPredicate::eq, operandBiasedExp, cNeg1);
  Value expectedOperandMaskedMantissa = b.create<arith::SelectOp>(
      operandBiasedExpEqNeg1, c0, safeShiftRight(c2To22, operandBiasedExp));
  Value operandMaskedMantissa = maskMantissa(operandBitcast, operandBiasedExp);
  Value operandIsHalfway =
      b.create<arith::CmpIOp>(arith::CmpIPredicate::eq, operandMaskedMantissa,
                              expectedOperandMaskedMantissa);
  // Ensure `biasedExp` is in the valid range for half values.
  Value operandBiasedExpGeNeg1 = b.create<arith::CmpIOp>(
      arith::CmpIPredicate::sge, operandBiasedExp, cNeg1);
  Value operandBiasedExpLt23 =
      b.create<arith::CmpIOp>(arith::CmpIPredicate::slt, operandBiasedExp, c23);
  operandIsHalfway =
      b.create<arith::AndIOp>(operandIsHalfway, operandBiasedExpLt23);
  operandIsHalfway =
      b.create<arith::AndIOp>(operandIsHalfway, operandBiasedExpGeNeg1);

  // Adjust rounded operand with `round(operand) - sign(operand)` to correct the
  // case where `round` rounded in the opposite direction of `roundeven`.
  Value sign = b.create<math::CopySignOp>(c1Float, operand);
  Value roundShifted = b.create<arith::SubFOp>(round, sign);
  // If the rounded value is even or a special value, we default to the behavior
  // of `math.round`.
  Value needsShift =
      b.create<arith::AndIOp>(roundIsNotEvenOrSpecialVal, operandIsHalfway);
  Value result = b.create<arith::SelectOp>(needsShift, roundShifted, round);
  // The `x - sign` adjustment does not preserve the sign when we are adjusting
  // the value -1 to -0. So here the sign is copied again to ensure that -0.5 is
  // rounded to -0.0.
  result = b.create<math::CopySignOp>(result, operand);
  rewriter.replaceOp(op, result);
  return success();
}

// Convert `math.rsqrt` into `arith.divf` + `math.sqrt`
static LogicalResult convertRsqrtOp(math::RsqrtOp op,
                                    PatternRewriter &rewriter) {

  auto operand = op.getOperand();
  auto operandTy = operand.getType();
  // Operand type must be shatic shaped type to create const float.
  auto shapedOperandType = dyn_cast<ShapedType>(operandTy);
  if (shapedOperandType && !shapedOperandType.hasStaticShape())
    return failure();

  auto eTy = getElementTypeOrSelf(operandTy);
  if (!isa<FloatType>(eTy))
    return failure();

  Location loc = op->getLoc();
  auto constOneFloat = createFloatConst(loc, operandTy, 1.0, rewriter);
  auto sqrtOp = rewriter.create<math::SqrtOp>(loc, operand);
  rewriter.replaceOpWithNewOp<arith::DivFOp>(op, constOneFloat, sqrtOp);
  return success();
}

void mlir::populateExpandCtlzPattern(RewritePatternSet &patterns) {
  patterns.add(convertCtlzOp);
}

void mlir::populateExpandSinhPattern(RewritePatternSet &patterns) {
  patterns.add(convertSinhOp);
}

void mlir::populateExpandCoshPattern(RewritePatternSet &patterns) {
  patterns.add(convertCoshOp);
}

void mlir::populateExpandTanPattern(RewritePatternSet &patterns) {
  patterns.add(convertTanOp);
}

void mlir::populateExpandTanhPattern(RewritePatternSet &patterns) {
  patterns.add(convertTanhOp);
}

void mlir::populateExpandAsinhPattern(RewritePatternSet &patterns) {
  patterns.add(convertAsinhOp);
}

void mlir::populateExpandAcoshPattern(RewritePatternSet &patterns) {
  patterns.add(convertAcoshOp);
}

void mlir::populateExpandAtanhPattern(RewritePatternSet &patterns) {
  patterns.add(convertAtanhOp);
}

void mlir::populateExpandFmaFPattern(RewritePatternSet &patterns) {
  patterns.add(convertFmaFOp);
}

void mlir::populateExpandCeilFPattern(RewritePatternSet &patterns) {
  patterns.add(convertCeilOp);
}

void mlir::populateExpandExp2FPattern(RewritePatternSet &patterns) {
  patterns.add(convertExp2fOp);
}

void mlir::populateExpandPowFPattern(RewritePatternSet &patterns) {
  patterns.add(convertPowfOp);
}

void mlir::populateExpandFPowIPattern(RewritePatternSet &patterns) {
  patterns.add(convertFPowIOp);
}

void mlir::populateExpandRoundFPattern(RewritePatternSet &patterns) {
  patterns.add(convertRoundOp);
}

void mlir::populateExpandRoundEvenPattern(RewritePatternSet &patterns) {
  patterns.add(convertRoundEvenOp);
}

void mlir::populateExpandRsqrtPattern(RewritePatternSet &patterns) {
  patterns.add(convertRsqrtOp);
}
