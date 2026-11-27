#include "fragment.glsl"

in vec3 vsNormal;
in vec2 vsTexCoord;
in vec4 vsPositionCameraSpace;

uniform vec3 sunDirection = vec3(0.0, 0.0, 1.0);
uniform vec3 buildingColor = vec3(0.82, 0.80, 0.76);
uniform float opacity = 1.0;

Fragment getFragment() {
  vec3 normal = normalize(vsNormal);
  float diffuse = max(dot(normal, normalize(sunDirection)), 0.25);
  vec3 litColor = buildingColor * diffuse;

  Fragment frag;
  frag.color = vec4(litColor, opacity);
  frag.depth = vsPositionCameraSpace.w;
  frag.gPosition = vsPositionCameraSpace;
  frag.gNormal = vec4(normal, 0.0);
  return frag;
}