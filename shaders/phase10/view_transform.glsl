struct GpuViewConstantsV1
{
    vec4 viewProjectionRows[4];
    vec4 previousViewProjectionRows[4];
    vec4 unjitteredViewProjectionRows[4];
    vec4 clipAndJitter;
    vec4 viewport;
    uvec4 identifiers;
};

layout(set = 0, binding = 6, std140) uniform ViewConstants
{
    GpuViewConstantsV1 record;
} viewConstants;

vec4 vfTransformPosition(vec3 position)
{
    vec4 homogeneous = vec4(position, 1.0);
    if (viewConstants.record.identifiers.x == 0u) {
        return homogeneous;
    }
    return vec4(
        dot(viewConstants.record.viewProjectionRows[0], homogeneous),
        dot(viewConstants.record.viewProjectionRows[1], homogeneous),
        dot(viewConstants.record.viewProjectionRows[2], homogeneous),
        dot(viewConstants.record.viewProjectionRows[3], homogeneous));
}
