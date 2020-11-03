import 'package:flutter/material.dart';
import 'package:nerves_example/thermostat.dart';

class ThermostatScreen extends StatefulWidget {
  @override
  _ThermostatScreenState createState() => _ThermostatScreenState();
}

class _ThermostatScreenState extends State<ThermostatScreen> {
  static const textColor = const Color(0xFFFFFFFD);

  bool _turnOn;

  @override
  void initState() {
    _turnOn = true;
    super.initState();
  }

  @override
  Widget build(BuildContext context) {
    return new Scaffold(
      body: new Container(
        decoration: BoxDecoration(
          color: const Color(0xFF0F2027),
          /*gradient: new LinearGradient(
            colors: [
              const Color(0xFF0F2027),
              const Color(0xFF2C5364),
            ],
            begin: Alignment.topRight,
            end: Alignment.bottomLeft,
          ),*/
        ),
        child: new SafeArea(
          child: new Column(
            children: <Widget>[
              new Expanded(
                child: new Center(
                  child: new Thermostat(
                    radius: 150.0,
                    turnOn: _turnOn,
                    modeIcon: Icon(
                      Icons.ac_unit,
                      color: Color(0xFF3CAEF4),
                    ),
                    textStyle: new TextStyle(
                      color: textColor,
                      fontSize: 34.0,
                    ),
                    minValue: 18,
                    maxValue: 38,
                    initialValue: 26,
                    onValueChanged: (value) {
                      print('Selected value : $value');
                    },
                  ),
                ),
              ),
              new Container(
                //width: double.infinity,
                height: 1.0,
                color: Colors.white.withOpacity(0.2),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class InfoIcon extends StatelessWidget {
  final Widget icon;
  final String text;

  const InfoIcon({Key key, this.icon, this.text}) : super(key: key);

  @override
  Widget build(BuildContext context) {
    return new Row(
      children: <Widget>[
        icon,
        new SizedBox(width: 8.0),
        new Text(
          text,
          style: const TextStyle(
            color: const Color(0xFFA9A6AF),
            fontSize: 12.0,
          ),
        ),
        new SizedBox(width: 12.0),
      ],
    );
  }
}

class BottomButton extends StatelessWidget {
  final Widget icon;
  final String text;
  final VoidCallback onTap;

  const BottomButton({
    Key key,
    this.icon,
    this.text,
    this.onTap,
  }) : super(key: key);

  @override
  Widget build(BuildContext context) {
    return new GestureDetector(
      onTap: onTap,
      child: new Column(
        mainAxisSize: MainAxisSize.min,
        children: <Widget>[
          new Container(
            width: 52.0,
            height: 52.0,
            margin: const EdgeInsets.only(bottom: 8.0),
            decoration: new BoxDecoration(
              shape: BoxShape.circle,
              border: Border.all(color: const Color(0xFF3F5BFA)),
            ),
            child: icon,
          ),
          new Text(
            text,
            style: const TextStyle(
              color: Colors.white,
              fontSize: 12.0,
            ),
          )
        ],
      ),
    );
  }
}

bool almostEqual(double a, double b, double eps) {
  return (a - b).abs() < eps;
}

bool angleBetween(
    double angle1, double angle2, double minTolerance, double maxTolerance) {
  final diff = (angle1 - angle2).abs();
  return diff >= minTolerance && diff <= maxTolerance;
}
