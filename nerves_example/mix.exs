defmodule NervesExample.MixProject do
  use Mix.Project

  @app :nerves_example
  @version "0.1.0"
  @all_targets [:rpi3]
  Mix.target(:rpi3)

  def project do
    [
      app: @app,
      name: "flutter_demo",
      product: "flutter_demo",
      version: @version,
      elixir: "~> 1.9",
      archives: [nerves_bootstrap: "~> 1.8"],
      start_permanent: Mix.env() == :prod,
      build_embedded: true,
      aliases: [loadconfig: [&bootstrap/1]],
      deps: deps(),
      releases: [{@app, release()}],
      preferred_cli_target: [run: :host, test: :host],
      compilers: Mix.compilers() ++ [:flutter_embedder]
    ]
  end

  # Starting nerves_bootstrap adds the required aliases to Mix.Project.config()
  # Aliases are only added if MIX_TARGET is set.
  def bootstrap(args) do
    Application.start(:nerves_bootstrap)
    Mix.Task.run("loadconfig", args)
  end

  # Run "mix help compile.app" to learn about applications.
  def application do
    [
      mod: {NervesExample.Application, []},
      extra_applications: [:logger, :runtime_tools]
    ]
  end

  # Run "mix help deps" to learn about dependencies.
  defp deps do
    [
      # Dependencies for all targets
      {:nerves, "~> 1.6.0 or ~> 1.7.0", runtime: false},
      {:shoehorn, "~> 0.6"},
      {:ring_logger, "~> 0.8"},
      {:toolshed, "~> 0.2"},
      {:nerves_hub_cli, "~> 0.10", runtime: false},

      # Dependencies for all targets except :host
      {:nerves_runtime, "~> 0.6", targets: @all_targets},
      {:nerves_pack, "~> 0.4", targets: @all_targets},
      {:nerves_hub_link, "~> 0.9", targets: @all_targets},
      {:nerves_key, "~> 0.5", targets: @all_targets},
      {:vintage_net_wizard, "~> 0.4.0", targets: @all_targets},
      {:nerves_key_pkcs11, "~> 0.2", targets: @all_targets},

      # Dependencies for specific targets
      # {:nerves_system_rpi3, "~> 1.13", targets: @all_targets},
      {
        :nerves_system_rpi3,
        #  path: "~/nerves/nerves_system_br/o/configs/nerves_system_rpi3_drm",
        path: "~/workspace/flutter/nerves_system_rpi3", runtime: false, targets: :rpi3
      },
      {:flutter_embedder, path: "../"},
      {:input_event, "~> 0.4.0", targets: @all_targets},
      {:nerves_system_br, "~> 1.13", override: true},
      {:power_control, github: "cjfreeze/power_control", targets: @all_targets}
    ]
  end

  def release do
    [
      overwrite: true,
      include_erts: &Nerves.Release.erts/0,
      steps: [&Nerves.Release.init/1, :assemble],
      strip_beams: Mix.env() == :prod
    ]
  end
end
