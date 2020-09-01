defmodule FlutterEmbedder.MixProject do
  use Mix.Project

  def project do
    [
      app: :flutter_embedder,
      version: "0.1.0",
      elixir: "~> 1.10",
      start_permanent: Mix.env() == :prod,
      compilers: Mix.compilers() ++ [:elixir_make],
      make_targets: ["all"],
      make_clean: ["clean"],
      make_cwd: "src",
      deps: deps()
    ]
  end

  # Run "mix help compile.app" to learn about applications.
  def application do
    [
      extra_applications: [:logger]
    ]
  end

  # Run "mix help deps" to learn about dependencies.
  defp deps do
    [
      {:elixir_make, "~> 0.6.0", runtime: false}
    ]
  end
end
