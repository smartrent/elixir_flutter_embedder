defmodule Example.Application do
  # See https://hexdocs.pm/elixir/Application.html
  # for more information on OTP Applications
  @moduledoc false

  use Application

  def start(_type, _args) do
    children = [
      {FlutterEmbedder,
       [Example.FlutterMethodHandler, Application.app_dir(:example, ["priv", "flutter_assets"])]}
      # GoveeBulb
      # Starts a worker by calling: Example.Worker.start_link(arg)
      # {Example.Worker, arg}
    ]

    # See https://hexdocs.pm/elixir/Supervisor.html
    # for other strategies and supported options
    opts = [strategy: :one_for_one, name: Example.Supervisor]
    Supervisor.start_link(children, opts)
  end
end
