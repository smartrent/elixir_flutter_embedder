defmodule FlutterEmbedder do
  alias FlutterEmbedder.{PlatformChannelMessage, StandardMessageCodec, StandardMethodCall}
  import StandardMessageCodec, only: [is_valid_dart_value: 1]
  defstruct [:port, :module, :uri]

  require Logger
  use GenServer, child_spec: false

  def child_spec(opts) do
    %{
      id: __MODULE__,
      start: {__MODULE__, :start_link, opts},
      type: :worker,
      restart: :permanent,
      shutdown: 500
    }
  end

  def start_link(standard_method_call_handler, flutter_assets, opts \\ []) do
    GenServer.start_link(__MODULE__, [standard_method_call_handler, flutter_assets], opts)
  end

  @impl GenServer
  def init([module | args]) do
    case sanity_check(args) do
      {:ok, args} ->
        Logger.info("#{port_executable()} #{Enum.join(args, " ")}")

        port =
          Port.open({:spawn_executable, port_executable()}, [
            {:args, args},
            :binary,
            :exit_status,
            # {:packet, 2},
            # :nouse_stdio,
            {:env,
             [{'LD_LIBRARY_PATH', to_charlist(Application.app_dir(:flutter_embedder, ["priv"]))}]}
          ])

        {:ok, %__MODULE__{module: module, port: port}}
    end
  end

  @impl true
  def terminate(_, state) do
    if state.port do
      Port.close(state.port)
    end

    remove_mdns_service(state)
  end

  @impl GenServer
  def handle_info({port, {:exit_status, status}}, %{port: port} = state) do
    {:stop, {:flutter_embedder_crash, status}, state}
  end

  def handle_info(
        {port, {:data, data = "flutter: Observatory listening on " <> uri}},
        %{port: port} = state
      ) do
    Logger.info("#{data}")
    uri = URI.parse(String.trim(uri))
    state = %{state | uri: uri}
    add_mdns_service(state)
    {:noreply, state}
  end

  def handle_info({port, {:data, data}}, %{port: port} = state) do
    Logger.info("#{data}")
    {:noreply, state}
  end

  # def handle_info({port, {:data, data}}, %{port: port} = state) do
  #   platform_channel_message = PlatformChannelMessage.decode(data)
  #   Logger.info("#{inspect(platform_channel_message)}")

  #   case StandardMethodCall.decode(platform_channel_message) do
  #     {:ok, call} ->
  #       handle_standard_call(platform_channel_message, call, state)

  #     {:error, reason} ->
  #       Logger.error("Could not decode data as StandardMethodCall: #{reason}")

  #       reply_bin =
  #         PlatformChannelMessage.encode_response(platform_channel_message, :not_implemented)

  #       true = Port.command(state.port, reply_bin)
  #       {:noreply, state}
  #   end
  # end

  def handle_standard_call(
        %PlatformChannelMessage{channel: channel} = call,
        %StandardMethodCall{method: method, args: args},
        state
      ) do
    case state.module.handle_std_call(channel, method, args) do
      {:ok, value} when is_valid_dart_value(value) ->
        value_ = StandardMessageCodec.encode_value(value)
        reply_bin = PlatformChannelMessage.encode_response(call, {:ok, value_})
        true = Port.command(state.port, reply_bin)

      {:error, code, message, value} ->
        code_ = StandardMessageCodec.encode_value(code)
        message_ = StandardMessageCodec.encode_value(message)
        value_ = StandardMessageCodec.encode_value(value)

        reply_bin =
          PlatformChannelMessage.encode_response(call, {:error, code_ <> message_ <> value_})

        true = Port.command(state.port, reply_bin)

      :not_implemented ->
        reply_bin = PlatformChannelMessage.encode_response(call, :not_implemented)
        true = Port.command(state.port, reply_bin)
    end

    {:noreply, state}
  end

  # TODO Check for errors instead of raising
  @doc false
  def sanity_check([flutter_assets]) do
    true = "vm_snapshot_data" in File.ls!(flutter_assets)
    icudtl_file = Application.app_dir(:flutter_embedder, ["priv", "icudtl.dat"])
    # icudtl_file = "icudtl.dat"
    {:ok, ["#{flutter_assets}", "#{icudtl_file}"]}
  end

  @doc false
  def port_executable() do
    hack = "/root/flutter_embedder"

    exe =
      if File.exists?(hack) do
        :ok = File.chmod(hack, 0o777)
        hack
      else
        Application.app_dir(:flutter_embedder, ["priv", "flutter_embedder"])
      end

    Logger.info("Using #{exe} for flutter_embedder")
    exe
  end

  def add_mdns_service(%{uri: uri}) do
    services = [
      %{
        name: "Flutter Observatory",
        protocol: "dartobservatory",
        transport: "tcp",
        port: uri.port,
        txt_payload: [URI.encode_query(%{path: uri.path, port: uri.port})]
      }
    ]

    MdnsLite.add_mdns_services(services)
  end

  def remove_mdns_service(_state) do
    MdnsLite.remove_mdns_services("Flutter Observatory")
  end
end
