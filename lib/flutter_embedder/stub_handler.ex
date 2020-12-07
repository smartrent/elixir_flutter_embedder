defmodule FlutterEmbedder.StubMethodCallHandler do
  def handle_std_call(channel, method, args) do
    IO.inspect({channel, method, args}, label: "unhandled method call")
    :not_implemented
  end
end
