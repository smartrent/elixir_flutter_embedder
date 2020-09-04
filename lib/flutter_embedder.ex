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
    case Jason.decode(data) do
      {:ok, json} ->
        IO.inspect(json, label: "not sure how to handle json yet...")
        {:noreply, state}

      {:error, _} ->
        call = FlutterEmbedder.StandardCall.decode(data)
        handle_standard_call(call, state)
    end
  end

  def handle_standard_call(call, state) do
    if call.channel == "platform/idk" do
      reply = 105.69
      value_bin = FlutterEmbedder.StandardCallCodec.encode_value(reply)
      reply_bin = <<call.handle::8, 0, value_bin::binary>>
      true = Port.command(state.port, <<byte_size(reply_bin)::16, reply_bin::binary>>)
    end

    {:noreply, state}
  end

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
