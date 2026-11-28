#include "fragment.glsl"

in vec3 vsNormal;
in vec2 vsTexCoord;
in vec4 vsPositionCameraSpace;

uniform vec3 sunDirection = vec3(0.0, 0.0, 1.0);
uniform vec3 buildingColor = vec3(0.84, 0.82, 0.78);
uniform float opacity = 1.0;

Fragment getFragment() {
  vec3 normal = normalize(vsNormal);
  float diffuse = max(dot(normal, normalize(sunDirection)), 0.35);

  float isRoof = smoothstep(0.5, 0.85, abs(dot(normal, normalize(sunDirection))));
  vec3 wallTint = buildingColor * vec3(0.82, 0.85, 0.90);
  vec3 baseColor = mix(wallTint, buildingColor, isRoof);

  Fragment frag;
  frag.color = vec4(baseColor * diffuse, opacity);
  frag.depth = vsPositionCameraSpace.w;
  frag.gPosition = vsPositionCameraSpace;
  frag.gNormal = vec4(normal, 0.0);
  return frag;
}