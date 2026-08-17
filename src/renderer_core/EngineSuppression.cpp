#include "renderer_core/EngineSuppression.h"

namespace vf::renderer::suppression {

DrawDisposition ClassifyDraw(const DrawContext& context) noexcept
{
    // Conjunction rather than a scored decision: each condition is a veto, and
    // a draw is dropped only when all three agree it is safe. Ordering is
    // irrelevant to the result and deliberately not relied upon.
    if (!context.permitGrants) return DrawDisposition::Vanilla;
    if (!context.writesWorldTarget) return DrawDisposition::Vanilla;
    if (!context.worldReproduced) return DrawDisposition::Vanilla;
    return DrawDisposition::Suppressed;
}

bool PresentationIsWhole(
    const bool worldSuppressed,
    const bool vulkanPresented) noexcept
{
    return worldSuppressed == vulkanPresented;
}

}
