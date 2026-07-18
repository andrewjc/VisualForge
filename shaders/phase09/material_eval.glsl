const float VF_PI = 3.14159265358979323846;

float vfSmithG1(float cosineValue, float alphaSquared)
{
    if (cosineValue <= 0.0) return 0.0;
    return 2.0 * cosineValue /
        (cosineValue + sqrt(alphaSquared +
            (1.0 - alphaSquared) * cosineValue * cosineValue));
}

vec3 vfEvaluateGgx(
    vec3 baseColor,
    vec3 normal,
    vec3 specularF0,
    float alphaRoughness,
    float fresnelPower,
    vec3 viewDirection,
    vec3 lightDirection,
    vec3 radiance)
{
    vec3 halfVector = normalize(viewDirection + lightDirection);
    float nDotV = max(dot(normal, viewDirection), 0.0);
    float nDotL = max(dot(normal, lightDirection), 0.0);
    float nDotH = max(dot(normal, halfVector), 0.0);
    float vDotH = max(dot(viewDirection, halfVector), 0.0);
    if (nDotV <= 0.0 || nDotL <= 0.0) return vec3(0.0);
    float alphaSquared = alphaRoughness * alphaRoughness;
    float denominator = nDotH * nDotH * (alphaSquared - 1.0) + 1.0;
    float distribution = alphaSquared /
        max(VF_PI * denominator * denominator, 1.0e-8);
    float geometry = vfSmithG1(nDotV, alphaSquared) *
        vfSmithG1(nDotL, alphaSquared);
    float fresnelAmount = pow(max(1.0 - vDotH, 0.0), fresnelPower);
    vec3 fresnel = specularF0 + (vec3(1.0) - specularF0) * fresnelAmount;
    vec3 specular = distribution * geometry * fresnel /
        max(4.0 * nDotV * nDotL, 1.0e-6);
    vec3 diffuse = baseColor * (vec3(1.0) - fresnel) / VF_PI;
    return (diffuse + specular) * radiance * nDotL;
}
