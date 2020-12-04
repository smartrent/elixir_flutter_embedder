defmodule Mix.Tasks.Flutter.Discover do
  use Mix.Task
  alias FlutterEmbedder.MDNSClient

  def json(uri) do
    %{
      name: "Nerves Flutter (#{uri.host})",
      request: "attach",
      deviceId: "flutter-tester",
      observatoryUri: to_string(%{uri | host: "localhost"}),
      type: "dart",
      program: "lib/main.dart"
    }
    |> Jason.encode!()
  end

  def run(_) do
    Mix.shell().info("Discovering devices via MDNS")

    with {:ok, pid} <- MDNSClient.start_link([]),
         {:ok, discovered} <- MDNSClient.discover(pid) do
      for uri <- discovered do
        cmd = "ssh -L #{uri.port}:localhost:#{uri.port} #{uri.host}"

        info = """
        =============================================================

          Found Flutter Observatory: #{uri.host}
              tunnel:      #{cmd}
              url:         #{to_string(%{uri | host: "localhost"})}
              launch.json: #{json(uri)}
        =============================================================
        """

        Mix.shell().info(info)
      end
    else
      {:error, reason} -> Mix.raise("Failed to discover via MDNS: #{inspect(reason)}")
    end
  end
end
