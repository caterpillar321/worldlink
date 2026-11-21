#version 300 es
#extension GL_ARB_shading_language_include : enable

precision highp float;
in vec4 v_color;
in vec2 v_texcoord;

uniform int skipCM;
uniform int sourceTF; // eTransferFunction
uniform int targetTF; // eTransferFunction
uniform mat4x2 targetPrimaries;

uniform vec2 topLeft;
uniform vec2 bottomRight;
uniform vec2 fullSize;
uniform float radius;
uniform float roundingPower;
uniform float range;
uniform float shadowPower;

#include "CM.glsl"

// SEKAI_GAUSSIAN_SHADOW: the upstream shadow was alpha = (distance from the outer edge / range)^power, so a corner was
// exactly as dark as an edge at the same distance — unlike a real blur (Windows DWM, GTK box-shadow), where a convex
// corner gets lighter because less of the window is near it. Draw a gaussian-blurred rounded box instead
// (Evan Wallace, "Fast Rounded Rectangle Shadows": exact along x with erf, 4 gaussian samples along y).
// sigma = range/4 and the box grows by one sigma (CSS "spread"), so 4 sigma = range ends inside the drawn quad and the
// straight-edge falloff stays close to the old one with render_power 3. shadowPower / roundingPower are not used.
vec2 sekaiErf(vec2 x) {
    vec2 s = sign(x), a = abs(x);
    x = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    x *= x;
    return s - s / (x * x);
}

float sekaiGaussian(float x, float sigma) {
    return exp(-(x * x) / (2.0 * sigma * sigma)) / (2.50662827463 * sigma);
}

float sekaiShadowX(float x, float y, float sigma, float corner, vec2 halfSize) {
    float delta    = min(halfSize.y - corner - abs(y), 0.0);
    float curved   = halfSize.x - corner + sqrt(max(0.0, corner * corner - delta * delta));
    vec2  integral = 0.5 + 0.5 * sekaiErf((x + vec2(-curved, curved)) * (0.70710678 / sigma));
    return integral.y - integral.x;
}

float sekaiShadow(vec2 lower, vec2 upper, vec2 point, float sigma, float corner) {
    vec2  center   = (lower + upper) * 0.5;
    vec2  halfSize = (upper - lower) * 0.5;
    point -= center;
    float low   = point.y - halfSize.y;
    float high  = point.y + halfSize.y;
    float start = clamp(-3.0 * sigma, low, high);
    float end   = clamp(3.0 * sigma, low, high);
    float step  = (end - start) / 4.0;
    float y     = start + step * 0.5;
    float value = 0.0;
    for (int i = 0; i < 4; i++) {
        value += sekaiShadowX(point.x, point.y - y, sigma, corner, halfSize) * sekaiGaussian(y, sigma) * step;
        y += step;
    }
    return value;
}

layout(location = 0) out vec4 fragColor;
void main() {

    vec4 pixColor = v_color;

    vec2 pixCoord = fullSize * v_texcoord;

    // the quad is the window box grown by `range` on every side; `radius` = range + window rounding
    float sigma  = max(range / 4.0, 0.5);
    float inset  = max(range - sigma, 0.0);
    float corner = max(radius - range, 0.0) + (range - inset);
    vec2  lower  = vec2(inset, inset);
    vec2  upper  = fullSize - vec2(inset, inset);
    corner       = min(corner, 0.5 * min(upper.x - lower.x, upper.y - lower.y));

    pixColor[3] = pixColor[3] * clamp(sekaiShadow(lower, upper, pixCoord, sigma, corner), 0.0, 1.0);

    if (pixColor[3] <= 0.001) {
        discard; return;
    }

    // premultiply
    pixColor.rgb *= pixColor[3];

	if (skipCM == 0)
        pixColor = doColorManagement(pixColor, sourceTF, targetTF, targetPrimaries);

	fragColor = pixColor;
}
