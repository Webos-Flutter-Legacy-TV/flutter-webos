// input_test: logs what remote keys reach Flutter on older webOS.
// Every key event (HardwareKeyboard), system back navigation (didPopRoute)
// and lifecycle change is written to <app dir>/compat_results.log as
// "[K] ...". The app starts with a second page pushed, so a working Back key
// first pops to the home page; Back on the home page lets the app exit.
import 'dart:io';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

void log(String line) {
  try {
    File('${File(Platform.resolvedExecutable).parent.path}/compat_results.log')
        .writeAsStringSync('[K] $line\n', mode: FileMode.append, flush: true);
  } catch (_) {}
}

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  HardwareKeyboard.instance.addHandler((KeyEvent e) {
    final kind = e is KeyDownEvent ? 'down' : e is KeyUpEvent ? 'up' : 'repeat';
    log('key $kind logical=${e.logicalKey.debugName}(0x${e.logicalKey.keyId.toRadixString(16)}) '
        'physical=${e.physicalKey.debugName}(0x${e.physicalKey.usbHidUsage.toRadixString(16)}) '
        'char=${e.character == null ? '-' : e.character!.codeUnits}');
    return false; // let Flutter handle it normally too
  });
  WidgetsBinding.instance.addObserver(_Observer());
  // Pointer events as Flutter receives them (Magic Remote pointer, buttons,
  // wheel): every event except hover/move, which is logged every 20th.
  var moves = 0;
  GestureBinding.instance.pointerRouter.addGlobalRoute((PointerEvent e) {
    final isMove = e is PointerHoverEvent || e is PointerMoveEvent;
    if (isMove && moves++ % 20 != 0) return;
    final extra = e is PointerScrollEvent
        ? ' scroll=${e.scrollDelta.dx.toStringAsFixed(0)},${e.scrollDelta.dy.toStringAsFixed(0)}'
        : '';
    log('pointer ${e.runtimeType} kind=${e.kind.name} at ${e.position.dx.toStringAsFixed(0)},'
        '${e.position.dy.toStringAsFixed(0)} buttons=${e.buttons}$extra${isMove ? ' (#$moves)' : ''}');
  });
  runApp(const MaterialApp(home: HomePage()));
}

class _Observer with WidgetsBindingObserver {
  @override
  Future<bool> didPopRoute() async {
    log('didPopRoute (system back)');
    return false; // let the Navigator handle it
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) => log('lifecycle $state');
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});
  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  @override
  void initState() {
    super.initState();
    log('start ${DateTime.now()}');
    WidgetsBinding.instance.addPostFrameCallback((_) {
      Navigator.of(context).push(MaterialPageRoute<void>(builder: (_) => const SecondPage()));
      log('pushed second page');
    });
  }

  @override
  Widget build(BuildContext context) {
    return PopScope(
      onPopInvokedWithResult: (didPop, _) => log('home pop invoked didPop=$didPop'),
      child: const Scaffold(body: Center(child: Text('Home page: Back exits', style: TextStyle(fontSize: 40)))),
    );
  }
}

class SecondPage extends StatelessWidget {
  const SecondPage({super.key});
  @override
  Widget build(BuildContext context) {
    return PopScope(
      onPopInvokedWithResult: (didPop, _) => log('second page popped didPop=$didPop'),
      child: Scaffold(
        appBar: AppBar(title: const Text('Second page')),
        body: const Center(child: Text('Second page: Back returns home', style: TextStyle(fontSize: 40))),
      ),
    );
  }
}
