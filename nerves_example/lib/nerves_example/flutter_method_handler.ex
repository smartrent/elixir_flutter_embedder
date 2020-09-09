defmodule NervesExample.FlutterMethodHandler do
  @behaviour FlutterEmbedder.StandardMethodCall.Handler

  def handle_std_call(_channel, _method, _value) do
    :not_implemented
  end
end
