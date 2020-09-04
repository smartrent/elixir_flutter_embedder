defmodule FlutterEmbedder do
  alias FlutterEmbedder.{PlatformChannelMessage, StandardMessageCodec, StandardCall}
  defstruct [:port]

  require Logger
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
            {:packet, 2},
            :nouse_stdio,
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

  def handle_info({port, {:data, data}}, %{port: port} = state) do
    case Jason.decode(data) do
      {:ok, json} ->
        Logger.error("not sure how to handle json yet... value: #{inspect(json, pretty: true)}")
        {:noreply, state}

      {:error, _} ->
        platform_channel_message = PlatformChannelMessage.decode(data)

        case StandardCall.decode(platform_channel_message) do
          {:ok, call} ->
            handle_standard_call(%{platform_channel_message | message: call}, state)

          {:error, reason} ->
            Logger.error("Could not decode data as StandardCall: #{reason}")
            {:noreply, state}
        end
    end
  end

  def handle_standard_call(call, state) do
    Logger.info("call: #{inspect(call)}")

    if call.channel == "platform/idk" do
      Logger.info("replying")
      value = StandardMessageCodec.encode_value(100.0)
      reply_bin = PlatformChannelMessage.encode_response(call, {:ok, value})
      true = Port.command(state.port, reply_bin)
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
end
