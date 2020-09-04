defmodule FlutterEmbedder do
  defstruct [:port]

  use GenServer

  def start_link(flutter_assets) do
    GenServer.start_link(__MODULE__, [flutter_assets], name: __MODULE__)
  end

  def send_packet(packet) do
    GenServer.cast(__MODULE__, {:send_packet, packet})
  end

  @impl GenServer
  def init(args) do
    case sanity_check(args) do
      {:ok, args} ->
        port =
          Port.open({:spawn_executable, port_executable()}, [
            {:args, args},
            :binary,
            :exit_status,
            # {:packet, 2},
            {:env,
             [{'LD_LIBRARY_PATH', to_charlist(Application.app_dir(:flutter_embedder, ["priv"]))}]}
          ])

        {:ok, %__MODULE__{port: port}}
    end
  end

  @impl GenServer
  def handle_cast({:send_packet, packet}, %{port: port} = state) do
    Port.command(port, packet)
    {:noreply, state}
  end

  @impl GenServer
  def handle_info({port, {:exit_status, status}}, %{port: port} = state) do
    {:stop, {:flutter_embedder_crash, status}, state}
  end

  # hack because packet: 2 isn't working
  def handle_info(
        {port, {:data, <<length::16, data::binary-size(length)>>}},
        %{port: port} = state
      ) do
    handle_info({port, {:data, data}}, state)
  end

  def handle_info({port, {:data, data}}, %{port: port} = state) do
    try do
      call =
        FlutterEmbedder.StandardCall.decode(data)
        |> IO.inspect()

      case handle_standard_call(call) do
        {:ok, reply} ->
          value_bin =
            FlutterEmbedder.StandardCallCodec.encode_value(reply)
            |> IO.inspect(label: "success encoded")

          reply_bin = <<call.handle::8, 0, value_bin::binary>>
          true = Port.command(port, <<byte_size(reply_bin)::16, reply_bin::binary>>)

        {:error, reason} ->
          class = FlutterEmbedder.StandardCallCodec.encode_value("idk")
          message = FlutterEmbedder.StandardCallCodec.encode_value(reason)
          reply_bin = <<call.handle::8, 1>> <> class <> message
          true = Port.command(port, <<byte_size(reply_bin)::16, reply_bin::binary>>)
      end
    catch
      error, reason ->
        IO.warn(
          """
          error: #{error}
          reason: #{inspect(reason)}
          """,
          __STACKTRACE__
        )

        :ok
    end

    {:noreply, state}
  end

  def handle_standard_call(%{channel: "platform/idk"}) do
    {:ok, 100.0}
  end

  def handle_standard_call(_), do: {:error, "unhandled"}

  # TODO Check for errors instead of raising
  defp sanity_check([flutter_assets]) do
    true = "vm_snapshot_data" in File.ls!(flutter_assets)
    # icudtl_file = Application.app_dir(:flutter_embedder, ["priv", "icudtl.dat"])
    icudtl_file = "icudtl.dat"
    {:ok, ["#{flutter_assets}", "#{icudtl_file}"]}
  end

  defp port_executable() do
    Application.app_dir(:flutter_embedder, ["priv", "flutter_embedder"])
  end

  # defp engine_executable() do
  #   Application.app_dir(:flutter_embedder, ["priv", "libflutter_engine.so"])
  # end
end
