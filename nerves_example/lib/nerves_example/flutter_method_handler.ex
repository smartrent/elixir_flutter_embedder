defmodule NervesExample.FlutterMethodHandler do
  @behaviour FlutterEmbedder.StandardMethodCall.Handler
  require Logger

  def handle_std_call(channel, method, value) do
    Logger.debug(inspect(%{channel: channel, method: method, value: value}))
    :not_implemented
  end
end
