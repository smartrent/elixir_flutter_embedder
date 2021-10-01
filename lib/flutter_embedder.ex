defmodule FlutterEmbedder do
  # @moduledoc File.read!("README.md")
  alias FlutterEmbedder.{PlatformChannelMessage, StandardMessageCodec, StandardMethodCall}
  import StandardMessageCodec, only: [is_valid_dart_value: 1]
  defstruct [:port, :uri, :module]

  require Logger
  use GenServer, child_spec: false

  def child_spec(opts) do
    %{
      id: __MODULE__,
      start: {__MODULE__, :start_link, [opts]},
      type: :worker,
      restart: :permanent,
      shutdown: 500
    }
  end

  def start_link(args, opts \\ [name: __MODULE__]) do
    GenServer.start_link(__MODULE__, args, opts)
  end

  def send_platform_message(embedder \\ __MODULE__, channel, data)
      when is_valid_dart_value(data) do
    GenServer.cast(embedder, {:send_platform_message, channel, data})
  end

  def observatory_url(embedder \\ __MODULE__) do
    GenServer.call(embedder, :observatory_url)
  end

  @impl GenServer
  def init(args) do
    case sanity_check(args) do
      {:ok, flutter_args} ->
        Logger.info("flutter args: #{port_executable()} #{Enum.join(flutter_args, " ")}")

        # LD_LIBRARY_PATH=/srv/erlang/lib/flutter_embedder-0.1.0/priv/ /srv/erlang/lib/flutter_embedder-0.1.0/priv/flutter_embedder /srv/erlang/lib/firmware-0.1.0/priv/flutter_assets /srv/erlang/lib/flutter_embedder-0.1.0/priv/icudtl.dat --disable-service-auth-codes --observatory-host 0.0.0.0 --observatory-port 43403 --disable-service-auth-codes --enable-service-port-fallback

        port =
          Port.open({:spawn_executable, port_executable()}, [
            {:args, flutter_args},
            :binary,
            :exit_status,
            {:packet, 4},
            # :nouse_stdio,
            {:env,
             [{'LD_LIBRARY_PATH', to_charlist(Application.app_dir(:flutter_embedder, ["priv"]))}]}
          ])

        {:ok,
         %__MODULE__{
           port: port,
           module: args[:method_call_handler] || FlutterEmbedder.StubMethodCallHandler
         }}
    end
  end

  @impl true
  def terminate(_, state) do
  end

  @impl GenServer
  def handle_info({port, {:exit_status, status}}, %{port: port} = state) do
    # {:stop, {:flutter_embedder_crash, status}, state}
    Logger.error("Flutter embedder crashed: #{status}")
    {:noreply, state}
  end

  def handle_info({port, {:data, <<1, log::binary>>}}, %{port: port} = state) do
    Logger.info(log)

    case log do
      "flutter: Observatory listening on " <> uri ->
        uri = URI.parse(String.trim(uri))
        state = %{state | uri: uri}
        {:noreply, state}

      _ ->
        {:noreply, state}
    end
  end

  def handle_info({port, {:data, <<2, log::binary>>}}, %{port: port} = state) do
    Logger.error(log)
    {:noreply, state}
  end

  def handle_info({port, {:data, data}}, %{port: port} = state) do
    platform_channel_message = PlatformChannelMessage.decode(data)
    Logger.info("incomming call #{inspect(platform_channel_message)}")

    case StandardMethodCall.decode(platform_channel_message) do
      {:ok, call} ->
        Logger.info("handling call: #{inspect(call)}")
        handle_standard_call(platform_channel_message, call, state)

      {:error, reason} ->
        Logger.error(
          "Could not decode #{platform_channel_message.channel} message as StandardMethodCall: #{reason} (this is probably ok)"
        )

        reply_bin =
          PlatformChannelMessage.encode_response(platform_channel_message, :not_implemented)

        true = Port.command(state.port, reply_bin)
        {:noreply, state}
    end
  end

  @impl GenServer
  def handle_cast({:send_platform_message, channel, data}, state) do
    message_bin =
      %FlutterEmbedder.PlatformChannelMessage{
        channel: channel,
        type: 0x0,
        message: <<0x0::8, FlutterEmbedder.StandardMessageCodec.encode_value(data)::binary>>,
        cookie: 255
      }
      |> FlutterEmbedder.PlatformChannelMessage.encode()

    true = Port.command(state.port, message_bin)
    {:noreply, state}
  end

  @impl GenServer
  def handle_call(:observatory_url, _from, state) do
    {:reply, state.uri, state}
  end

  def handle_standard_call(
        %PlatformChannelMessage{channel: channel} = call,
        %StandardMethodCall{method: method, args: args},
        state
      ) do
    case state.module.handle_std_call(channel, method, args) |> IO.inspect(label: "reply") do
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

      error ->
        Logger.error(
          "Failed to handle response from message handler: invalid value: #{inspect(error)}"
        )
    end

    {:noreply, state}
  end

  # TODO Check for errors instead of raising
  @doc false
  def sanity_check(args) do
    flutter_assets =
      args[:flutter_assets] || raise ArgumentError, "`flutter_assets` is a required argument"

    true = "vm_snapshot_data" in File.ls!(flutter_assets)

    icudtl_file =
      args[:icudtl_file] || Application.app_dir(:flutter_embedder, ["priv", "icudtl.dat"])

    {:ok,
     [
       "#{flutter_assets}",
       "#{icudtl_file}",
       "--disable-service-auth-codes",
       "--observatory-host",
       "0.0.0.0",
       "--observatory-port",
       "43403",
       "--disable-service-auth-codes",
       "--enable-service-port-fallback"
     ]}
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
end
