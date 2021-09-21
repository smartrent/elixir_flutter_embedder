defmodule FlutterEmbedder.StubMethodCallHandler do
  require Logger
  def handle_std_call(channel, method, args) do
    Logger.debug "unhandled method call: #{inspect(channel)} #{inspect(method)} #{inspect(args)}"
    :not_implemented
  end
end
