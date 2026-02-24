variable "N00B_PLATFORMS" {
  default = "linux/amd64,linux/arm64"
}

variable "N00B_LOCAL_PLATFORM" {
  default = "linux/amd64"
}

variable "N00B_VERIFY_TAG" {
  default = "n00b:cross-platform-verify"
}

variable "N00B_WINDOWS_CROSS_PLATFORMS" {
  default = "linux/amd64"
}

variable "N00B_WINDOWS_CROSS_LOCAL_PLATFORM" {
  default = "linux/amd64"
}

variable "N00B_WINDOWS_CROSS_VERIFY_TAG" {
  default = "n00b:windows-cross-verify"
}

target "_verify_common" {
  context    = "."
  dockerfile = "docker/Dockerfile"
  target     = "verify"
  pull       = true
  tags       = ["${N00B_VERIFY_TAG}"]
  cache-from = ["type=local,src=.buildx-cache"]
  cache-to   = ["type=local,dest=.buildx-cache-new,mode=max"]
  output     = ["type=cacheonly"]
}

target "_verify_windows_cross_common" {
  context    = "."
  dockerfile = "docker/Dockerfile"
  target     = "verify-windows-cross"
  pull       = true
  tags       = ["${N00B_WINDOWS_CROSS_VERIFY_TAG}"]
  cache-from = ["type=local,src=.buildx-cache"]
  cache-to   = ["type=local,dest=.buildx-cache-new,mode=max"]
  output     = ["type=cacheonly"]
}

target "verify-local" {
  inherits  = ["_verify_common"]
  platforms = ["${N00B_LOCAL_PLATFORM}"]
}

target "verify" {
  inherits  = ["_verify_common"]
  platforms = split(",", N00B_PLATFORMS)
}

target "verify-windows-cross-local" {
  inherits  = ["_verify_windows_cross_common"]
  platforms = ["${N00B_WINDOWS_CROSS_LOCAL_PLATFORM}"]
}

target "verify-windows-cross" {
  inherits  = ["_verify_windows_cross_common"]
  platforms = split(",", N00B_WINDOWS_CROSS_PLATFORMS)
}

group "default" {
  targets = ["verify-local"]
}
