/*
 * Copyright 2020 Google LLC.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/ganesh/tessellate/shaders/GrStrokeTessellationShader.h"

#include "src/gpu/ganesh/glsl/GrGLSLFragmentShaderBuilder.h"
#include "src/gpu/ganesh/glsl/GrGLSLVertexGeoBuilder.h"
#include "src/gpu/tessellate/FixedCountBufferUtils.h"
#include "src/gpu/tessellate/WangsFormula.h"

using skgpu::VertexWriter;
using skgpu::FixedCountStrokes;

void GrStrokeTessellationShader::InstancedImpl::onEmitCode(EmitArgs& args, GrGPArgs* gpArgs) {
    const auto& shader = args.fGeomProc.cast<GrStrokeTessellationShader>();
    SkPaint::Join joinType = shader.stroke().getJoin();
    args.fVaryingHandler->emitAttributes(shader);

    args.fVertBuilder->defineConstant("float", "PI", "3.141592653589793238");
    args.fVertBuilder->defineConstant("PRECISION", skgpu::kTessellationPrecision);

    // There is an artificial maximum number of edges (compared to the max limit calculated based on
    // the number of radial segments per radian, Wang's formula, and join type). When there is
    // vertex ID support, the limit is what can be represented in a uint16; otherwise the limit is
    // the size of the fallback vertex buffer.
    float maxEdges = args.fShaderCaps->vertexIDSupport() ? FixedCountStrokes::kMaxEdges
                                                         : FixedCountStrokes::kMaxEdgesNoVertexIDs;
    args.fVertBuilder->defineConstant("NUM_TOTAL_EDGES", maxEdges);

    // Helper functions.
    if (shader.hasDynamicStroke()) {
        args.fVertBuilder->insertFunction(kNumRadialSegmentsPerRadianFn);
    }
    args.fVertBuilder->insertFunction(kRobustNormalizeDiffFn);
    args.fVertBuilder->insertFunction(kCosineBetweenUnitVectorsFn);
    args.fVertBuilder->insertFunction(kMiterExtentFn);
    args.fVertBuilder->insertFunction(kUncheckedMixFn);
    args.fVertBuilder->insertFunction(GrTessellationShader::WangsFormulaSkSL());

    // Tessellation control uniforms and/or dynamic attributes.
    if (!shader.hasDynamicStroke()) {
        // [MAX_SCALE, NUM_RADIAL_SEGMENTS_PER_RADIAN, JOIN_TYPE, STROKE_RADIUS]
        const char* tessArgsName;
        fTessControlArgsUniform = args.fUniformHandler->addUniform(
                nullptr, kVertex_GrShaderFlag, SkSLType::kFloat4, "tessControlArgs",
                &tessArgsName);
        args.fVertBuilder->codeAppendf(R"(
        float MAX_SCALE = %s.x;
        float NUM_RADIAL_SEGMENTS_PER_RADIAN = %s.y;
        float JOIN_TYPE = %s.z;
        float STROKE_RADIUS = %s.w;)", tessArgsName, tessArgsName, tessArgsName, tessArgsName);
    } else {
        const char* maxScaleName;
        fTessControlArgsUniform = args.fUniformHandler->addUniform(
                nullptr, kVertex_GrShaderFlag, SkSLType::kFloat, "maxScale",
                &maxScaleName);
        args.fVertBuilder->codeAppendf(R"(
        float MAX_SCALE = %s;
        float STROKE_RADIUS = dynamicStrokeAttr.x;
        float NUM_RADIAL_SEGMENTS_PER_RADIAN = num_radial_segments_per_radian(
                MAX_SCALE, STROKE_RADIUS);
        float JOIN_TYPE = dynamicStrokeAttr.y;)", maxScaleName);
    }

    if (shader.hasDynamicColor()) {
        // Create a varying for color to get passed in through.
        GrGLSLVarying dynamicColor{SkSLType::kHalf4};
        args.fVaryingHandler->addVarying("dynamicColor", &dynamicColor);
        args.fVertBuilder->codeAppendf("%s = dynamicColorAttr;", dynamicColor.vsOut());
        fDynamicColorName = dynamicColor.fsIn();
    }


    // View matrix uniforms.
    const char* translateName, *affineMatrixName;
    fAffineMatrixUniform = args.fUniformHandler->addUniform(nullptr, kVertex_GrShaderFlag,
                                                            SkSLType::kFloat4, "affineMatrix",
                                                            &affineMatrixName);
    fTranslateUniform = args.fUniformHandler->addUniform(nullptr, kVertex_GrShaderFlag,
                                                         SkSLType::kFloat2, "translate",
                                                         &translateName);
    args.fVertBuilder->codeAppendf("float2x2 AFFINE_MATRIX = float2x2(%s);\n", affineMatrixName);
    args.fVertBuilder->codeAppendf("float2 TRANSLATE = %s;\n", translateName);

    if (shader.hasExplicitCurveType()) {
        args.fVertBuilder->insertFunction(SkStringPrintf(R"(
        bool is_conic_curve() { return curveTypeAttr != %g; })", skgpu::kCubicCurveType).c_str());
    } else {
        args.fVertBuilder->insertFunction(R"(
        bool is_conic_curve() { return isinf(pts23Attr.w); })");
    }

    // Tessellation code.
    args.fVertBuilder->codeAppend(R"(
    float2 p0=pts01Attr.xy, p1=pts01Attr.zw, p2=pts23Attr.xy, p3=pts23Attr.zw;
    float2 lastControlPoint = argsAttr.xy;
    float w = -1;  // w<0 means the curve is an integral cubic.
    if (is_conic_curve()) {
        // Conics are 3 points, with the weight in p3.
        w = p3.x;
        p3 = p2;  // Setting p3 equal to p2 works for the remaining rotational logic.
    })");

    // Emit code to call Wang's formula to determine parametric segments. We do this before
    // transform points for hairlines so that it is consistent with how the CPU tested the control
    // points for chopping.
    args.fVertBuilder->codeAppend(R"(
    // Find how many parametric segments this stroke requires.
    float numParametricSegments;
    if (w < 0) {
        if (p0 == p1 && p2 == p3) {
            numParametricSegments = 1; // a line
        } else {
            numParametricSegments = wangs_formula_cubic(PRECISION, p0, p1, p2, p3, AFFINE_MATRIX);
        }
    } else {
        numParametricSegments = wangs_formula_conic(PRECISION,
                                                    AFFINE_MATRIX * p0,
                                                    AFFINE_MATRIX * p1,
                                                    AFFINE_MATRIX * p2, w);
    })");

    if (shader.stroke().isHairlineStyle()) {
        // Hairline case. Transform the points before tessellation. We can still hold off on the
        // translate until the end; we just need to perform the scale and skew right now.
        args.fVertBuilder->codeAppend(R"(
        p0 = AFFINE_MATRIX * p0;
        p1 = AFFINE_MATRIX * p1;
        p2 = AFFINE_MATRIX * p2;
        p3 = AFFINE_MATRIX * p3;
        lastControlPoint = AFFINE_MATRIX * lastControlPoint;)");
    }

    args.fVertBuilder->codeAppend(R"(
    // Find the starting and ending tangents.
    // (p0 == p1) ? ((p1 == p2) ? p3 : p2) : p1
    float2 tan0 = robust_normalize_diff((p0 == p1) ? ((p1 == p2) ? p3 : p2) : p1, p0);
    float2 tan1 = robust_normalize_diff(p3, (p3 == p2) ? ((p2 == p1) ? p0 : p1) : p2);
    if (tan0 == float2(0)) {
        // The stroke is a point. This special case tells us to draw a stroke-width circle as a
        // 180 degree point stroke instead.
        tan0 = float2(1,0);
        tan1 = float2(-1,0);
    })");

    if (args.fShaderCaps->vertexIDSupport()) {
        // If we don't have sk_VertexID support then "edgeID" already came in as a vertex attrib.
        args.fVertBuilder->codeAppend(R"(
        float edgeID = float(sk_VertexID >> 1);
        if ((sk_VertexID & 1) != 0) {
            edgeID = -edgeID;
        })");
    }

    // Potential optimization: (shader.hasDynamicStroke() && shader.hasRoundJoins())?
    if (shader.stroke().getJoin() == SkPaint::kRound_Join || shader.hasDynamicStroke()) {
        args.fVertBuilder->codeAppend(R"(
        // Determine how many edges to give to the round join. We emit the first and final edges
        // of the join twice: once full width and once restricted to half width. This guarantees
        // perfect seaming by matching the vertices from the join as well as from the strokes on
        // either side.
        float2 prevTan = robust_normalize_diff(p0, lastControlPoint);
        float joinRads = acos(cosine_between_unit_vectors(prevTan, tan0));
        float numRadialSegmentsInJoin = max(ceil(joinRads * NUM_RADIAL_SEGMENTS_PER_RADIAN), 1);
        // +2 because we emit the beginning and ending edges twice (see above comment).
        float numEdgesInJoin = numRadialSegmentsInJoin + 2;
        // The stroke section needs at least two edges. Don't assign more to the join than
        // "NUM_TOTAL_EDGES - 2". (This is only relevant when the ideal max edge count calculated
        // on the CPU had to be limited to NUM_TOTAL_EDGES in the draw call).
        numEdgesInJoin = min(numEdgesInJoin, NUM_TOTAL_EDGES - 2);)");
        if (shader.mode() == GrStrokeTessellationShader::Mode::kLog2Indirect) {
            args.fVertBuilder->codeAppend(R"(
            // Negative argsAttr.z means the join is an internal chop or circle, and both of
            // those have empty joins. All we need is a bevel join.
            if (argsAttr.z < 0) {
                // +2 because we emit the beginning and ending edges twice (see above comment).
                numEdgesInJoin = 1 + 2;
            })");
        }
        if (shader.hasDynamicStroke()) {
            args.fVertBuilder->codeAppend(R"(
            if (JOIN_TYPE >= 0 /*Is the join not a round type?*/) {
                // Bevel and miter joins get 1 and 2 segments respectively.
                // +2 because we emit the beginning and ending edges twice (see above comments).
                numEdgesInJoin = sign(JOIN_TYPE) + 1 + 2;
            })");
        }
    } else {
        args.fVertBuilder->codeAppendf(R"(
        float numEdgesInJoin = %i;)",
        skgpu::NumFixedEdgesInJoin(joinType));
    }

    args.fVertBuilder->codeAppend(R"(
    // Find which direction the curve turns.
    // NOTE: Since the curve is not allowed to inflect, we can just check F'(.5) x F''(.5).
    // NOTE: F'(.5) x F''(.5) has the same sign as (P2 - P0) x (P3 - P1)
    float turn = cross_length_2d(p2 - p0, p3 - p1);
    float combinedEdgeID = abs(edgeID) - numEdgesInJoin;
    if (combinedEdgeID < 0) {
        tan1 = tan0;
        // Don't let tan0 become zero. The code as-is isn't built to handle that case. tan0=0
        // means the join is disabled, and to disable it with the existing code we can leave
        // tan0 equal to tan1.
        if (lastControlPoint != p0) {
            tan0 = robust_normalize_diff(p0, lastControlPoint);
        }
        turn = cross_length_2d(tan0, tan1);
    }

    // Calculate the curve's starting angle and rotation.
    float cosTheta = cosine_between_unit_vectors(tan0, tan1);
    float rotation = acos(cosTheta);
    if (turn < 0) {
        // Adjust sign of rotation to match the direction the curve turns.
        rotation = -rotation;
    }

    float numRadialSegments;
    float strokeOutset = sign(edgeID);
    if (combinedEdgeID < 0) {
        // We belong to the preceding join. The first and final edges get duplicated, so we only
        // have "numEdgesInJoin - 2" segments.
        numRadialSegments = numEdgesInJoin - 2;
        numParametricSegments = 1;  // Joins don't have parametric segments.
        p3 = p2 = p1 = p0;  // Colocate all points on the junction point.
        // Shift combinedEdgeID to the range [-1, numRadialSegments]. This duplicates the first
        // edge and lands one edge at the very end of the join. (The duplicated final edge will
        // actually come from the section of our strip that belongs to the stroke.)
        combinedEdgeID += numRadialSegments + 1;
        // We normally restrict the join on one side of the junction, but if the tangents are
        // nearly equivalent this could theoretically result in bad seaming and/or cracks on the
        // side we don't put it on. If the tangents are nearly equivalent then we leave the join
        // double-sided.
        float sinEpsilon = 1e-2;  // ~= sin(180deg / 3000)
        bool tangentsNearlyParallel =
                (abs(turn) * inversesqrt(dot(tan0, tan0) * dot(tan1, tan1))) < sinEpsilon;
        if (!tangentsNearlyParallel || dot(tan0, tan1) < 0) {
            // There are two edges colocated at the beginning. Leave the first one double sided
            // for seaming with the previous stroke. (The double sided edge at the end will
            // actually come from the section of our strip that belongs to the stroke.)
            if (combinedEdgeID >= 0) {
                strokeOutset = (turn < 0) ? min(strokeOutset, 0) : max(strokeOutset, 0);
            }
        }
        combinedEdgeID = max(combinedEdgeID, 0);
    } else {
        // We belong to the stroke. Unless NUM_RADIAL_SEGMENTS_PER_RADIAN is incredibly high,
        // clamping to maxCombinedSegments will be a no-op because the draw call was invoked with
        // sufficient vertices to cover the worst case scenario of 180 degree rotation.
        float maxCombinedSegments = NUM_TOTAL_EDGES - numEdgesInJoin - 1;
        numRadialSegments = max(ceil(abs(rotation) * NUM_RADIAL_SEGMENTS_PER_RADIAN), 1);
        numRadialSegments = min(numRadialSegments, maxCombinedSegments);
        numParametricSegments = min(numParametricSegments,
                                    maxCombinedSegments - numRadialSegments + 1);
    }

    // Additional parameters for emitTessellationCode().
    float radsPerSegment = rotation / numRadialSegments;
    float numCombinedSegments = numParametricSegments + numRadialSegments - 1;
    bool isFinalEdge = (combinedEdgeID >= numCombinedSegments);
    if (combinedEdgeID > numCombinedSegments) {
        strokeOutset = 0;  // The strip has more edges than we need. Drop this one.
    })");

    if (joinType == SkPaint::kMiter_Join || shader.hasDynamicStroke()) {
        args.fVertBuilder->codeAppendf(R"(
        // Edge #2 extends to the miter point.
        if (abs(edgeID) == 2 && %s) {
            strokeOutset *= miter_extent(cosTheta, JOIN_TYPE/*miterLimit*/);
        })", shader.hasDynamicStroke() ? "JOIN_TYPE > 0/*Is the join a miter type?*/" : "true");
    }

    this->emitTessellationCode(shader, &args.fVertBuilder->code(), gpArgs, *args.fShaderCaps);

    this->emitFragmentCode(shader, args);
}
