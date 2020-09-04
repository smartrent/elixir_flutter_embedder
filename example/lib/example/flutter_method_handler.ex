defmodule Example.FlutterMethodHandler do
  require Logger
  @behaviour FlutterEmbedder.StandardMethodCall.Handler

  def handle_std_call("platform/idk", "getBatteryLevel", _) do
    # {:ok, 100.0}
    # :not_implemented
    {:error, "anyolethang", "The vehicle doesn't support the PID used for this channel.", nil}
  end

  def handle_std_call(channel, method, value) do
    Logger.debug("""
    Flutter platform message not implemented:
    channel: #{channel}
    method: #{method}
    value: #{inspect(value)}
    """)

    :not_implemented
  end
end
