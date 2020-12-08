defmodule FlutterEmbedder.StubMethodCallHandler do
  require Logger

  def handle_std_call("samples.flutter.io/battery", "getBatteryLevel", _args) do
    {:ok, 69}
  end

  def handle_std_call(channel, method, args) do
    Logger.error("Unhandled std method call #{channel}:#{method}(#{inspect(args)}")
    :not_implemented
  end
end
