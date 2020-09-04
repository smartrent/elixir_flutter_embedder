defmodule Example.FlutterMethodHandler do
  @behaviour FlutterEmbedder.StandardMethodCall.Handler

  def handle_std_call("samples.flutter.dev/battery", "getBatteryLevel", _) do
    {:ok, 100.00}
  end

  def handle_std_call(_channel, _method, _value) do
    :not_implemented
  end
end
