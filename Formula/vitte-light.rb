class VitteLight < Formula
  desc "Lightweight runtime and CLI for the Vitte language"
  homepage "https://github.com/vitte-lang/VitteLight"
  url "https://github.com/vitte-lang/VitteLight/archive/refs/tags/v0.2.0.tar.gz"
  sha256 "REPLACE_WITH_REAL_SHA256"
  license "MIT"

  depends_on "cmake" => :build

  def install
    system "cmake", "-S", ".", "-B", "build", *std_cmake_args,
           "-DCMAKE_BUILD_TYPE=Release",
           "-DCMAKE_C_STANDARD=11"
    system "cmake", "--build", "build", "--parallel"
    system "cmake", "--install", "build"
  end

  test do
    # Check that the CLI responds to --help
    assert_match(/--help/, shell_output("#{bin}/vitte-cli --help"))
  end
end