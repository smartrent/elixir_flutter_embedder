defmodule Example.MixProject do
  use Mix.Project

  def project do
    [
      app: :example,
      version: "0.1.0",
      elixir: "~> 1.10",
      start_permanent: Mix.env() == :prod,
      compilers: Mix.compilers() ++ [:flutter_embedder],
      flutter_project: "thermostat",
      deps: deps()
    ]
  end

  # Run "mix help compile.app" to learn about applications.
  def application do
    [
      extra_applications: [:logger],
      mod: {Example.Application, []}
    ]
  end

  # Run "mix help deps" to learn about dependencies.
  defp deps do
    [
      {:flutter_embedder, path: "../"},
      {:blue_heron, "~> 0.1"},
      {:blue_heron_transport_usb, "~> 0.1.0"}
    ]
  end
end
