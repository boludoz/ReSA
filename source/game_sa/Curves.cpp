#include "Curves.h"
#include "common.h"

void CCurves::InjectHooks() {
    RH_ScopedClass(CCurves);
    RH_ScopedCategoryGlobal();

    RH_ScopedGlobalInstall(TestCurves, 0x43C600, {.locked=true});
    RH_ScopedGlobalInstall(DistForLineToCrossOtherLine, 0x43C610);
    RH_ScopedGlobalInstall(CalcSpeedVariationInBend, 0x43C660);
    RH_ScopedGlobalInstall(CalcSpeedScaleFactor, 0x43C710);
    RH_ScopedGlobalInstall(CalcCorrectedDist, 0x43C880);
    RH_ScopedGlobalInstall(CalcCurvePoint, 0x43C900);
}

// 0x43C610 - inlined
// Modified to use CVector2D instead of individual floats
float CCurves::DistForLineToCrossOtherLine(CVector2D lineBase, CVector2D lineDir, CVector2D otherLineBase, CVector2D otherLineDir) {
    if (const auto crossAB = lineDir.Cross(otherLineDir); crossAB != 0.0f) {
        return (lineBase - otherLineBase).Cross(otherLineDir) * (-1.0f / crossAB);
    }
    return -1.0f;
}

// 0x43C660 - inlined
// Modified to use CVector2D for direction parameters
float CCurves::CalcSpeedVariationInBend(const CVector &startCoors, const CVector &endCoors, const CVector2D StartDir, const CVector2D EndDir) {
    // Calculate dot product between direction vectors
    float dotProduct = StartDir.Dot(EndDir);

    if (dotProduct <= 0.0f) {
        return ONE_THIRD;
    } else if (dotProduct <= 0.7f) {
        return (dotProduct / -0.7f) * ONE_THIRD + ONE_THIRD;
    }

    // Calculate distance from a line to a point, scaled by the distance between endpoints
    return (CCollision::DistToMathematicalLine2D(endCoors.x, endCoors.y, EndDir.x, EndDir.y, startCoors.x, startCoors.y) / (startCoors - endCoors).Magnitude2D()) * ONE_THIRD;
}

// 0x43C880 - inlined
float CCurves::CalcCorrectedDist(float current, float total, float speedVariation, float &pInterPol) {
    if (total >= EPSILON) {
        // Calculate corrected distance using sine wave variation
        float correctedDist = (total / TWO_PI * speedVariation * std::sin(current * TWO_PI / total)) + (((1.0f - 2.0f * speedVariation) * 0.5f + 0.5f) * current);
        // Calculate interpolation parameter using cosine
        pInterPol = 0.5f - (std::cos(current / total * PI) * 0.5f);
        return correctedDist;
    } else {
        // Handle case when total distance is too small
        pInterPol = 0.5f;
        return 0.0f;
    }
}

// 0x43C710 - not inlined
float CCurves::CalcSpeedScaleFactor(const CVector &startCoors, const CVector &endCoors, const CVector2D StartDir, const CVector2D EndDir) {
    // Calculate intersection distances for both lines
    float distOne = DistForLineToCrossOtherLine(CVector2D(startCoors.x, startCoors.y), StartDir, CVector2D(endCoors.x, endCoors.y), EndDir);
    float distTwo = DistForLineToCrossOtherLine(CVector2D(endCoors.x, endCoors.y), EndDir, CVector2D(startCoors.x, startCoors.y), StartDir);
    return (distOne <= 0.0f || distTwo >= 0.0f) ? std::hypot(startCoors.x - endCoors.x, startCoors.y - endCoors.y) / (1.0f - CalcSpeedVariationInBend(startCoors, endCoors, StartDir, EndDir)) : distOne - distTwo;
}

// 0x43C900 - not inlined
void CCurves::CalcCurvePoint(const CVector &startCoors, const CVector &endCoors, const CVector &startDir, const CVector &endDir, float time, int32 traversalTimeInMillis, CVector &resultCoor, CVector &resultSpeed) {
    
    float scaleFactor, interpFactor;

    // Calculate intersection parameters - Already check zero
    float distToCrossA = DistForLineToCrossOtherLine(CVector2D(startCoors), CVector2D(startDir), CVector2D(endCoors), CVector2D(endDir));
    float distToCrossB = -DistForLineToCrossOtherLine(CVector2D(endCoors), CVector2D(endDir), CVector2D(startCoors), CVector2D(startDir));

    // Clamp time between 0 and 1
    time = std::clamp(time, 0.0f, 1.0f);

    // If rays don't intersect properly, fall back to a simpler curved path approximation
    // This happens when the directions would never cross or are almost parallel
    if (distToCrossA <= 0.0f || distToCrossB <= 0.0f) {
        // Lines are parallel, use speed variation method
        const float speedVariation = CalcSpeedVariationInBend(startCoors, endCoors, CVector2D(startDir), CVector2D(endDir));
        scaleFactor = (startCoors - endCoors).Magnitude2D();
        const float speedScale = scaleFactor / (1.0f - speedVariation);
        const auto correctedDist = CalcCorrectedDist(time * speedScale, speedScale, speedVariation, interpFactor);
        resultCoor = lerp((startCoors + startDir * correctedDist), (endCoors + endDir * (correctedDist - scaleFactor)), interpFactor);
    } else {
        // For properly intersecting rays, create a three-segment path:
        // 1. Straight segment from start
        // 2. Curved bend in the middle
        // 3. Straight segment to end

        // Limit how sharp the bend can be for natural movement
        const float bendDistOneSegment = std::min({distToCrossA, distToCrossB, 5.0f});

        const float straightDistA = distToCrossA - bendDistOneSegment;
        const float straightDistB = distToCrossB - bendDistOneSegment;
        const float curveSegment = 2.0f * bendDistOneSegment;
        scaleFactor = straightDistA + curveSegment + straightDistB;
        const float currDist = time * scaleFactor;

        if (currDist < straightDistA) {
            // 1. Position is on the first straight segment (linear interpolation from start)
            resultCoor = startCoors + startDir * currDist;
        } else if (currDist > (straightDistA + curveSegment)) {
            // 2. Position is on the final straight segment (linear interpolation to end)
            resultCoor = endCoors + endDir * (currDist - scaleFactor);
        } else {
            // 3. Position is in the curved bend section - requires double interpolation
            //    First interpolate through the bend progress, then between the influenced points
            const float bendT = (currDist - straightDistA) / curveSegment;
    
            // Blend between influence points that extendoutward
            // from the bend endpoints to create a curved path
            resultCoor = lerp(
                startCoors + (startDir * straightDistA) + (startDir * (bendDistOneSegment * bendT)),
                endCoors - (endDir * straightDistB) - (endDir * (bendDistOneSegment * (1.0f - bendT))),
                bendT
            );
        }

    }

    // We dont need lambda - scale the interpolated direction by scaleFactor/time
    resultSpeed = lerp(startDir, endDir, time) * (scaleFactor / std::max(traversalTimeInMillis * 0.001f, EPSILON));
    resultSpeed.z = 0.0f; // Zero out vertical speed component
}

// unused
// 0x43C600
void CCurves::TestCurves() {

    // Test the CalcCurvePoint comparison - this is the main test we need
    NOTSA_LOG_DEBUG("Testing CalcCurvePoint against CCurves::CalcCurvePoint...");

    // Define test cases with various input parameters
    struct TestCase {
        CVector start, end, startDir, endDir;
        float time;
        int32 traversalTime;
        const char *name;
        CVector expectedPosition;
        CVector expectedSpeed;
    };

    auto printVector = [](const CVector &v) { 
        NOTSA_LOG_DEBUG("({}, {}, {})", v.x, v.y, v.z);
    };

    auto vectorsAlmostEqual = [](const CVector &v1, const CVector &v2, float tolerance) -> bool {
        return std::abs(v1.x - v2.x) < tolerance && std::abs(v1.y - v2.y) < tolerance && std::abs(v1.z - v2.z) < tolerance;
    };

    TestCase testCases[] = {
        // Straight line
        {CVector(0.0f, 0.0f, 0.0f), CVector(10.0f, 0.0f, 0.0f), CVector(1.0f, 0.0f, 0.0f), CVector(1.0f, 0.0f, 0.0f), 0.5f, 1000, "Straight line", CVector(5.0f, 0.0f, 0.0f),
         CVector(10.0f, 0.0f, 0.0f)},
        // 90-degree curve
        {CVector(0.0f, 0.0f, 0.0f), CVector(10.0f, 10.0f, 0.0f), CVector(1.0f, 0.0f, 0.0f), CVector(0.0f, 1.0f, 0.0f), 0.5f, 1000, "90-degree curve", CVector(8.75f, 1.25f, 0.0f),
         CVector(10.0f, 10.0f, 0.0f)},
        // S-curve
        {CVector(0.0f, 0.0f, 0.0f), CVector(10.0f, 0.0f, 0.0f), CVector(1.0f, 1.0f, 0.0f), CVector(1.0f, -1.0f, 0.0f), 0.5f, 2000, "S-curve", CVector(5.0f, 2.5f, 0.0f), CVector(5.0f, 0.0f, 0.0f)},
        // Opposite directions
        {CVector(0.0f, 0.0f, 0.0f), CVector(10.0f, 0.0f, 0.0f), CVector(1.0f, 0.0f, 0.0f), CVector(-1.0f, 0.0f, 0.0f), 0.5f, 1500, "Opposite directions", CVector(10.0f, 0.0f, 0.0f),
         CVector(0.0f, 0.0f, 0.0f)},
        // With Z component
        {CVector(0.0f, 0.0f, 0.0f), CVector(10.0f, 10.0f, 5.0f), CVector(1.0f, 0.0f, 0.5f), CVector(0.0f, 1.0f, 0.5f), 0.5f, 1000, "With Z component", CVector(8.75f, 1.25f, 2.5f),
         CVector(10.0f, 10.0f, 0.0f)},
        // Time at beginning
        {CVector(0.0f, 0.0f, 0.0f), CVector(10.0f, 10.0f, 0.0f), CVector(1.0f, 0.0f, 0.0f), CVector(0.0f, 1.0f, 0.0f), 0.0f, 1000, "Time at beginning", CVector(0.0f, 0.0f, 0.0f),
         CVector(20.0f, 0.0f, 0.0f)},
        // Time at end
        {CVector(0.0f, 0.0f, 0.0f), CVector(10.0f, 10.0f, 0.0f), CVector(1.0f, 0.0f, 0.0f), CVector(0.0f, 1.0f, 0.0f), 1.0f, 1000, "Time at end", CVector(10.0f, 10.0f, 0.0f),
         CVector(0.0f, 20.0f, 0.0f)},
        // Large values
        {CVector(1000.0f, 2000.0f, 100.0f), CVector(2000.0f, 1000.0f, 200.0f), CVector(1.0f, -0.5f, 0.1f), CVector(-0.5f, -1.0f, 0.1f), 0.5f, 5000, "Large values", CVector(1800.0f, 1600.0f, 180.0f),
         CVector(80.0f, -240.0f, 0.0f)}};

    constexpr float compareTolerance = 0.01f;
    int passed = 0;
    int total = sizeof(testCases) / sizeof(testCases[0]);

    for (const auto &test : testCases) {
        CVector resultCoor2, resultSpeed2;

        // Call both implementations with the same inputs
        CVector resultCoor1 = test.expectedPosition;
        CVector resultSpeed1 = test.expectedSpeed;

        CCurves::CalcCurvePoint(test.start, test.end, test.startDir, test.endDir, test.time, test.traversalTime, resultCoor2, resultSpeed2);

        // Compare results
        bool coordsMatch = vectorsAlmostEqual(resultCoor1, resultCoor2, compareTolerance);
        bool speedsMatch = vectorsAlmostEqual(resultSpeed1, resultSpeed2, compareTolerance);

        NOTSA_LOG_DEBUG("Test: {} - {}", test.name, (coordsMatch && speedsMatch) ? "PASSED" : "FAILED");
        
        NOTSA_LOG_DEBUG("  Expected position: ");
        printVector(resultCoor1);
        NOTSA_LOG_DEBUG("  Actual position:   ");
        printVector(resultCoor2);

        NOTSA_LOG_DEBUG("  Expected speed:    ");
        printVector(resultSpeed1);
        NOTSA_LOG_DEBUG("  Actual speed:      ");
        printVector(resultSpeed2);
        
        if (coordsMatch && speedsMatch) {
            passed++;
        }
    }

    NOTSA_LOG_DEBUG("CalcCurvePoint comparison test: {}/{} tests passed.", passed, total);

    assert(passed == total);
}
