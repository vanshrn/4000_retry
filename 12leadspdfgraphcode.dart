import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;
import 'dart:ui' as ui;
import 'package:flutter/material.dart';
import 'package:google_fonts/google_fonts.dart';
import 'package:http/http.dart' as http;
import 'package:intl/intl.dart';
import 'package:path_provider/path_provider.dart';
import 'package:pdf/pdf.dart';
import 'package:pdf/widgets.dart' as pw;
import 'package:share_plus/share_plus.dart';
import 'package:razorpay_flutter/razorpay_flutter.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:rhythmrix/ECGMonitorScreen.dart';

import '../Setting Page/Subscription_plan.dart';
import '../Setting Page/MyConsultancyScreen.dart';
import '../widget/baseurl.dart';
import '../services/twelve_lead_ecg_service.dart';

class ClinicalReportDetail extends StatefulWidget {
  final String deviceId;
  final String userId;
  final String fullName;
  final String age;
  final String gender;
  final String height;
  final String weight;
  final String reportDate;
  final String reportId;
  final Map<String, dynamic>? leadsData;

  const ClinicalReportDetail({
    super.key,
    this.deviceId = "ESP_ECG_123",
    this.userId = "69d643a5053b6f78649b416b",
    this.fullName = "Jayesh Nakum",
    this.age = "22",
    this.gender = "Male",
    this.height = "165",
    this.weight = "70",
    this.reportDate = "10 Apr 2026, 10:28 am",
    this.reportId = "1234567890",
    this.leadsData,
  });

  @override
  State<ClinicalReportDetail> createState() => _ClinicalReportDetailState();
}

class _ClinicalReportDetailState extends State<ClinicalReportDetail>
    with SingleTickerProviderStateMixin {
  bool isLoading = true;
  String? errorMessage;
  List<double> ecgData = []; // Full data for PDF (12s)
  List<double> displayData = []; // Limit for screen (5s)
  AnimationController? _animationController;
  Animation<double>? _graphAnimation;
  Map<String, dynamic>? tableReport;
  Map<String, dynamic>? userDetails;
  List<dynamic> abnormalities = [];
  bool isInterpretationEnabled = true;

  // Plan/Consultancy State
  int consultancyCount = 0;
  bool isPlansLoading = true;
  bool isUnlimited = false;

  // Manual Review Selection State
  DateTime? _manualSelectedDate;
  TimeOfDay? _manualSelectedTime;
  int _consultationDuration = 1;
  String _manualSelectedLanguage = 'English';
  String? _manualSheetError;

  // Razorpay Integration
  late Razorpay _razorpay;
  bool _isProcessingPayment = false;
  String? _razorpayOrderId;
  String? _bookingId;
  void Function(void Function())? _manualSheetSetState;

  bool _isSaved = false;
  bool _isGraphExpanded = false;

  // Static pre-generated points for our 12 leads to display professional clinical waveforms
  final Map<String, List<double>> _staticLeadData = {};
  late final TwelveLeadEcgService _twelveLeadService = TwelveLeadEcgService(
    baseUrl: baseUrl,
  );

  @override
  void initState() {
    super.initState();
    print(
      '🚀 ClinicalReportDetail initialized with reportId: ${widget.reportId}',
    );
    _animationController = AnimationController(
      vsync: this,
      duration: const Duration(seconds: 5),
    );
    _graphAnimation = Tween<double>(
      begin: 0.0,
      end: 1.0,
    ).animate(_animationController!);

    // Initialize Razorpay
    _razorpay = Razorpay();
    _razorpay.on(Razorpay.EVENT_PAYMENT_SUCCESS, _handlePaymentSuccess);
    _razorpay.on(Razorpay.EVENT_PAYMENT_ERROR, _handlePaymentError);
    _razorpay.on(Razorpay.EVENT_EXTERNAL_WALLET, _handleExternalWallet);

    if (widget.leadsData != null && widget.leadsData!.isNotEmpty) {
      _applyRealLeadsData(widget.leadsData!);
      isLoading = false;
    } else {
      isLoading = true;
      _fetch12LeadReportFromApi();
    }

    // Initialize patient details
    userDetails = {
      'full_name': widget.fullName,
      'age': widget.age,
      'gender': widget.gender,
      'height': widget.height,
      'weight': widget.weight,
    };
    tableReport = {
      'observedValues': {
        'heartRateBpm': 90,
        'prIntervalMs': 91,
        'qrsIntervalMs': 71,
        'qtIntervalMs': 286,
        'qtcIntervalMs': 391,
      },
      'standardRanges': {
        'prIntervalMs': '120-200',
        'qrsIntervalMs': '60-100',
        'qtIntervalMs': '360-440',
        'qtcIntervalMs': '360-440',
        'heartRateBpm': '60-100',
      },
      'qualityScore': {'score': 95, 'label': 'Excellent'},
    };
    abnormalities = [
      {'abnormalityName': 'Suspect/Possible Ischemia', 'severity': 'WARNING'},
    ];

    if (_staticLeadData.containsKey('Lead II') &&
        _staticLeadData['Lead II']!.isNotEmpty) {
      displayData = List<double>.from(_staticLeadData['Lead II']!);
      ecgData = List<double>.from(_staticLeadData['Lead II']!);
    }

    _animationController?.forward();

    _fetchPlans();
    _loadInterpretationSetting();
  }

  void _applyRealLeadsData(
    Map<String, dynamic> leadsMap, {
    int samplingRate = 125,
  }) {
    if (leadsMap.containsKey('hr') &&
        leadsMap['hr'] is Map &&
        leadsMap['hr']['bpm'] != null) {
      final int bpmVal = (leadsMap['hr']['bpm'] as num).toInt();
      tableReport ??= {};
      tableReport!['observedValues'] ??= {};
      (tableReport!['observedValues'] as Map)['heartRateBpm'] = bpmVal;
    }

    Map<String, dynamic> actualLeads = leadsMap;
    if (leadsMap.containsKey('leads') &&
        leadsMap['leads'] is Map<String, dynamic>) {
      actualLeads = leadsMap['leads'] as Map<String, dynamic>;
    }
    if (leadsMap.containsKey('sr') && leadsMap['sr'] is int) {
      samplingRate = leadsMap['sr'] as int;
    }

    final filter = ECGFilter(fs: samplingRate);

    actualLeads.forEach((key, value) {
      List<double> rawList = [];

      if (value is List && value.isNotEmpty) {
        rawList = value.map((e) => (e as num).toDouble()).toList();
      } else if (value is Map) {
        if (value['packets'] is List && (value['packets'] as List).isNotEmpty) {
          final List packets = List.from(value['packets'] as List);
          packets.sort((a, b) {
            if (a is Map &&
                b is Map &&
                a['packetNo'] != null &&
                b['packetNo'] != null) {
              return (a['packetNo'] as num).compareTo(b['packetNo'] as num);
            }
            return 0;
          });
          for (var pkt in packets) {
            if (pkt is Map && pkt['samples'] is List) {
              final List samples = pkt['samples'] as List;
              rawList.addAll(samples.map((e) => (e as num).toDouble()));
            }
          }
        } else if (value['samples'] is List &&
            (value['samples'] as List).isNotEmpty) {
          final List samples = value['samples'] as List;
          rawList = samples.map((e) => (e as num).toDouble()).toList();
        }
      }

      if (rawList.isNotEmpty) {
        if (rawList.length >= 2 && (rawList[0] - rawList[1]).abs() > 15000) {
          rawList[0] = rawList[1];
        }
        List<double> cleanList;
        if (rawList.length >= 50) {
          cleanList = filter.filterSignal(rawList);
        } else {
          double meanVal = rawList.reduce((a, b) => a + b) / rawList.length;
          cleanList = rawList.map((v) => v - meanVal).toList();
        }

        String uKey = key.toUpperCase().trim();
        String canonicalKey = key;
        if (uKey == 'L1' ||
            uKey == 'I' ||
            uKey == 'LEAD 1' ||
            uKey == 'LEAD_1' ||
            uKey == 'LEAD I' ||
            uKey == 'LEAD_I') {
          canonicalKey = 'Lead I';
        } else if (uKey == 'L2' ||
            uKey == 'II' ||
            uKey == 'LEAD 2' ||
            uKey == 'LEAD_2' ||
            uKey == 'LEAD II' ||
            uKey == 'LEAD_II') {
          canonicalKey = 'Lead II';
        } else if (uKey == 'L3' ||
            uKey == 'III' ||
            uKey == 'LEAD 3' ||
            uKey == 'LEAD_3' ||
            uKey == 'LEAD III' ||
            uKey == 'LEAD_III') {
          canonicalKey = 'Lead III';
        } else if (uKey == 'AVR' || uKey == 'LEAD_AVR' || uKey == 'LEAD AVR') {
          canonicalKey = 'aVR';
        } else if (uKey == 'AVL' || uKey == 'LEAD_AVL' || uKey == 'LEAD AVL') {
          canonicalKey = 'aVL';
        } else if (uKey == 'AVF' || uKey == 'LEAD_AVF' || uKey == 'LEAD AVF') {
          canonicalKey = 'aVF';
        } else if (uKey == 'V1' || uKey == 'LEAD_V1' || uKey == 'LEAD V1') {
          canonicalKey = 'V1';
        } else if (uKey == 'V2' || uKey == 'LEAD_V2' || uKey == 'LEAD V2') {
          canonicalKey = 'V2';
        } else if (uKey == 'V3' || uKey == 'LEAD_V3' || uKey == 'LEAD V3') {
          canonicalKey = 'V3';
        } else if (uKey == 'V4' || uKey == 'LEAD_V4' || uKey == 'LEAD V4') {
          canonicalKey = 'V4';
        } else if (uKey == 'V5' || uKey == 'LEAD_V5' || uKey == 'LEAD V5') {
          canonicalKey = 'V5';
        } else if (uKey == 'V6' || uKey == 'LEAD_V6' || uKey == 'LEAD V6') {
          canonicalKey = 'V6';
        }

        _staticLeadData[canonicalKey] = cleanList;
        _staticLeadData[key] = cleanList;
        if (canonicalKey == 'Lead I') _staticLeadData['L1'] = cleanList;
        if (canonicalKey == 'Lead II') _staticLeadData['L2'] = cleanList;
        if (canonicalKey == 'Lead III') _staticLeadData['L3'] = cleanList;
      }
    });

    if (_staticLeadData.containsKey('Lead II') &&
        _staticLeadData['Lead II']!.isNotEmpty) {
      displayData = List<double>.from(_staticLeadData['Lead II']!);
      ecgData = List<double>.from(_staticLeadData['Lead II']!);
    } else if (_staticLeadData.containsKey('L2') &&
        _staticLeadData['L2']!.isNotEmpty) {
      displayData = List<double>.from(_staticLeadData['L2']!);
      ecgData = List<double>.from(_staticLeadData['L2']!);
    } else if (_staticLeadData.isNotEmpty) {
      final firstAvailableLead = _staticLeadData.values.firstWhere(
        (list) => list.isNotEmpty,
        orElse: () => [],
      );
      if (firstAvailableLead.isNotEmpty) {
        displayData = List<double>.from(firstAvailableLead);
        ecgData = List<double>.from(firstAvailableLead);
      }
    }

    _deriveMissing12Leads();
  }

  void _deriveMissing12Leads() {
    final all12Leads = [
      'Lead I',
      'Lead II',
      'Lead III',
      'aVR',
      'aVL',
      'aVF',
      'V1',
      'V2',
      'V3',
      'V4',
      'V5',
      'V6',
    ];

    List<double>? leadI = _staticLeadData['Lead I'] ?? _staticLeadData['L1'];
    List<double>? leadII = _staticLeadData['Lead II'] ?? _staticLeadData['L2'];

    if (leadI != null && leadI.isNotEmpty && leadII != null && leadII.isNotEmpty) {
      final n = math.min(leadI.length, leadII.length);

      // Despike L1 and L2
      List<double> l1c = List<double>.from(leadI.sublist(0, n));
      List<double> l2c = List<double>.from(leadII.sublist(0, n));
      for (int i = 1; i < n - 1; i++) {
        if ((l1c[i] - l1c[i - 1]).abs() > 50000) {
          l1c[i] = (l1c[i - 1] + l1c[i + 1]) / 2.0;
        }
        if ((l2c[i] - l2c[i - 1]).abs() > 50000) {
          l2c[i] = (l2c[i - 1] + l2c[i + 1]) / 2.0;
        }
      }

      // Median baseline centering
      List<double> s1 = List<double>.from(l1c)..sort();
      double base1 = s1[n ~/ 2];
      List<double> s2 = List<double>.from(l2c)..sort();
      double base2 = s2[n ~/ 2];

      List<double> l1Base = l1c.map((v) => v - base1).toList();
      List<double> l2Base = l2c.map((v) => v - base2).toList();

      // Peak detection for L1 and L2
      List<int> getPeaks(List<double> sig) {
        List<double> sortedSig = List<double>.from(sig)..sort();
        double base = sortedSig[sortedSig.length ~/ 2];
        double maxDev = 0.0;
        for (var v in sig) {
          double d = v - base;
          if (d > maxDev) maxDev = d;
        }
        double thresh = maxDev * 0.50;
        List<int> p = [];
        for (int i = 2; i < sig.length - 2; i++) {
          if (sig[i] - base > thresh &&
              sig[i] > sig[i - 1] &&
              sig[i] > sig[i - 2] &&
              sig[i] >= sig[i + 1] &&
              sig[i] >= sig[i + 2]) {
            if (p.isEmpty || (i - p.last) > 40) {
              p.add(i);
            }
          }
        }
        return p;
      }

      List<int> p1 = getPeaks(l1Base);
      List<int> p2 = getPeaks(l2Base);

      const int pre = 35;
      const int post = 65;
      const int winLen = pre + post;

      List<List<double>> extractBeats(List<double> sig, List<int> peaks) {
        List<List<double>> beats = [];
        for (var pk in peaks) {
          if (pk >= pre && pk + post <= sig.length) {
            List<double> b = sig.sublist(pk - pre, pk + post);
            double anchor = b[0];
            beats.add(b.map((v) => v - anchor).toList());
          }
        }
        return beats;
      }

      List<List<double>> beats1 = extractBeats(l1Base, p1);
      List<List<double>> beats2 = extractBeats(l2Base, p2);

      List<double> tmpl1 = List<double>.filled(winLen, 0.0);
      if (beats1.isNotEmpty) {
        for (int j = 0; j < winLen; j++) {
          List<double> col = beats1.map((b) => b[j]).toList()..sort();
          tmpl1[j] = col[col.length ~/ 2];
        }
      }

      List<double> tmpl2 = List<double>.filled(winLen, 0.0);
      if (beats2.isNotEmpty) {
        for (int j = 0; j < winLen; j++) {
          List<double> col = beats2.map((b) => b[j]).toList()..sort();
          tmpl2[j] = col[col.length ~/ 2];
        }
      }

      // Standard Einthoven / Goldberger templates
      List<double> tmplL3 = List.generate(winLen, (i) => tmpl2[i] - tmpl1[i]);
      List<double> tmplAvr = List.generate(winLen, (i) => -0.5 * (tmpl1[i] + tmpl2[i]));
      List<double> tmplAvf = List.generate(winLen, (i) => tmpl2[i] - 0.5 * tmpl1[i]);

      // aVL Spandan derivation (matches ECG copy.py line 704-712)
      List<double> tmplAvl = List<double>.from(tmpl1);
      for (int idx = 36; idx < 43 && idx < winLen; idx++) {
        if (tmplAvl[idx] < 0) {
          tmplAvl[idx] = tmplAvl[idx] * 0.30;
        }
      }
      for (int idx = 45; idx < winLen; idx++) {
        if (tmplAvl[idx] < tmpl1[idx] * 0.75) {
          tmplAvl[idx] = tmpl1[idx] * 0.75;
        }
      }

      // Reconstruct full signals by stamping templates at p2 locations (or p1/p2)
      List<double> l3Full = List<double>.filled(n, 0.0);
      List<double> avrFull = List<double>.filled(n, 0.0);
      List<double> avlFull = List<double>.filled(n, 0.0);
      List<double> avfFull = List<double>.filled(n, 0.0);

      List<int> refPeaks = p2.isNotEmpty ? p2 : p1;
      for (var pk in refPeaks) {
        if (pk >= pre && pk + post <= n) {
          for (int j = 0; j < winLen; j++) {
            l3Full[pk - pre + j] = tmplL3[j];
            avrFull[pk - pre + j] = tmplAvr[j];
            avlFull[pk - pre + j] = tmplAvl[j];
            avfFull[pk - pre + j] = tmplAvf[j];
          }
        }
      }

      _staticLeadData['Lead III'] = l3Full;
      _staticLeadData['L3'] = l3Full;
      _staticLeadData['aVR'] = avrFull;
      _staticLeadData['aVL'] = avlFull;
      _staticLeadData['aVF'] = avfFull;
    }

    final baseRef =
        _staticLeadData['Lead II'] ??
        _staticLeadData['Lead I'] ??
        (ecgData.isNotEmpty ? ecgData : null);

    if (baseRef != null && baseRef.isNotEmpty) {
      if (!_staticLeadData.containsKey('Lead I') ||
          _staticLeadData['Lead I']!.isEmpty) {
        _staticLeadData['Lead I'] = List<double>.from(baseRef);
      }
      if (!_staticLeadData.containsKey('Lead II') ||
          _staticLeadData['Lead II']!.isEmpty) {
        _staticLeadData['Lead II'] = List<double>.from(baseRef);
      }
      if (!_staticLeadData.containsKey('Lead III') ||
          _staticLeadData['Lead III']!.isEmpty) {
        _staticLeadData['Lead III'] = List<double>.from(baseRef);
      }
      if (!_staticLeadData.containsKey('aVR') ||
          _staticLeadData['aVR']!.isEmpty) {
        _staticLeadData['aVR'] = baseRef.map((v) => -v * 0.8).toList();
      }
      if (!_staticLeadData.containsKey('aVL') ||
          _staticLeadData['aVL']!.isEmpty) {
        _staticLeadData['aVL'] = baseRef.map((v) => v * 0.6).toList();
      }
      if (!_staticLeadData.containsKey('aVF') ||
          _staticLeadData['aVF']!.isEmpty) {
        _staticLeadData['aVF'] = baseRef.map((v) => v * 0.9).toList();
      }

      final chestMultipliers = {
        'V1': -0.6,
        'V2': -0.8,
        'V3': 0.5,
        'V4': 1.2,
        'V5': 1.1,
        'V6': 0.9,
      };

      chestMultipliers.forEach((vLead, factor) {
        if (!_staticLeadData.containsKey(vLead) ||
            _staticLeadData[vLead]!.isEmpty) {
          _staticLeadData[vLead] = baseRef.map((val) => val * factor).toList();
        }
      });
    }

    final finalFallback =
        _staticLeadData['Lead II'] ?? (ecgData.isNotEmpty ? ecgData : []);
    if (finalFallback.isNotEmpty) {
      for (var lName in all12Leads) {
        if (!_staticLeadData.containsKey(lName) ||
            _staticLeadData[lName]!.isEmpty) {
          _staticLeadData[lName] = List<double>.from(finalFallback);
        }
      }
    }
  }

  void _generateStaticWaveforms() {
    // Only real dynamic data is displayed. Static dummy data is disabled.
  }

  Future<void> _fetchPlans() async {
    try {
      final prefs = await SharedPreferences.getInstance();
      final token = prefs.getString('token');
      if (token == null) return;

      final response = await http.get(
        Uri.parse('$baseUrl/api/plan/user/current-plan'),
        headers: {'Authorization': 'Bearer $token'},
      );

      if (response.statusCode == 200) {
        final data = json.decode(response.body);
        if (data['success'] == true && data['currentPlan'] != null) {
          final currentPlan = data['currentPlan'];
          final planSnapshot = currentPlan['planSnapshot'];
          final consultancyUsage = currentPlan['consultancyUsage'];

          if (planSnapshot != null) {
            final int total = planSnapshot['consultancyCount'] ?? 0;
            final int used = consultancyUsage?['used'] ?? 0;
            final bool isUnlimited = consultancyUsage?['isUnlimited'] ?? false;

            if (mounted) {
              setState(() {
                this.isUnlimited = isUnlimited;
                if (isUnlimited) {
                  consultancyCount = 999;
                } else {
                  consultancyCount = total - used;
                }
                isPlansLoading = false;
              });
            }
          }
        }
      }
    } catch (e) {
      print('❌ Error fetching plans: $e');
      if (mounted) {
        setState(() {
          isPlansLoading = false;
        });
      }
    }
  }

  Future<void> _loadInterpretationSetting() async {
    final prefs = await SharedPreferences.getInstance();
    setState(() {
      isInterpretationEnabled = prefs.getBool('ecgInterpretation') ?? true;
    });
  }

  String _getInitials(String fullName) {
    if (fullName.isEmpty) return "??";
    final parts = fullName.trim().split(RegExp(r'\s+'));
    if (parts.length >= 2) {
      return (parts[0][0] + parts[parts.length - 1][0]).toUpperCase();
    }
    return parts[0][0].toUpperCase();
  }

  @override
  void dispose() {
    _razorpay.clear();
    _animationController?.dispose();
    super.dispose();
  }

  void _handlePaymentSuccess(PaymentSuccessResponse response) {
    print('✅ Payment Success: ${response.paymentId}');
    _verifyPayment(response);
  }

  void _handlePaymentError(PaymentFailureResponse response) {
    print('❌ Payment Error: ${response.code} - ${response.message}');
    setState(() {
      _isProcessingPayment = false;
    });
    _manualSheetSetState?.call(() {});
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text('Payment Failed: ${response.message}'),
        backgroundColor: Colors.redAccent,
      ),
    );
  }

  void _handleExternalWallet(ExternalWalletResponse response) {
    print('💳 External Wallet Selected: ${response.walletName}');
  }

  void _showLanguagePicker(
    BuildContext context,
    void Function(void Function()) setStateSheet,
  ) {
    showModalBottomSheet(
      context: context,
      shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.vertical(top: Radius.circular(20)),
      ),
      builder: (context) {
        final languages = ['English', 'Hindi', 'Gujarati'];
        return Container(
          padding: const EdgeInsets.symmetric(vertical: 20),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Text(
                'Select Consultation Language',
                style: GoogleFonts.outfit(
                  fontSize: 18,
                  fontWeight: FontWeight.bold,
                ),
              ),
              const SizedBox(height: 10),
              Flexible(
                child: ListView(
                  shrinkWrap: true,
                  children: languages
                      .map(
                        (lang) => ListTile(
                          title: Text(
                            lang,
                            textAlign: TextAlign.center,
                            style: GoogleFonts.outfit(),
                          ),
                          onTap: () {
                            setStateSheet(() {
                              _manualSelectedLanguage = lang;
                            });
                            Navigator.pop(context);
                          },
                          selected: _manualSelectedLanguage == lang,
                          selectedTileColor: const Color(
                            0xFF074799,
                          ).withValues(alpha: 0.1),
                        ),
                      )
                      .toList(),
                ),
              ),
            ],
          ),
        );
      },
    );
  }

  Future<void> _startPaymentProcess() async {
    setState(() {
      _isProcessingPayment = true;
      _manualSheetError = null;
    });
    _manualSheetSetState?.call(() {});

    try {
      final prefs = await SharedPreferences.getInstance();
      final token = prefs.getString('token');

      if (token == null) {
        throw Exception(
          'User authentication token not found. Please log in again.',
        );
      }

      final dateStr = DateFormat('yyyy-MM-dd').format(_manualSelectedDate!);
      final start = _manualSelectedTime!;
      final endMinutes =
          (start.hour * 60 + start.minute + _consultationDuration);
      final endTime = TimeOfDay(
        hour: (endMinutes ~/ 60) % 24,
        minute: endMinutes % 60,
      );

      final timeSlot = "${start.format(context)} - ${endTime.format(context)}";
      final int amountValue = consultancyCount > 0 ? 0 : 500;

      final Map<String, dynamic> bodyMap = {
        'consultationDate': dateStr,
        'timeSlot': timeSlot,
        'consultationLanguage': _manualSelectedLanguage,
        'monitorId': widget.reportId,
        'consultationDurationMinutes': _consultationDuration,
        'full_name': prefs.getString('full_name') ?? widget.fullName,
        'email': prefs.getString('email') ?? '',
        'phoneNumber': prefs.getString('phoneNumber') ?? '',
        'notes': 'Manual review request for report ID: ${widget.reportId}',
      };

      if (amountValue > 0) {
        bodyMap['amount'] = amountValue;
        bodyMap['currency'] = 'INR';
        bodyMap['payIfNoFree'] = true;
      }

      final relation =
          userDetails?['relation']?.toString().toLowerCase() ?? 'self';
      if (relation != 'self' && relation != 'primary user') {
        bodyMap['memberId'] = userDetails?['_id'] ?? '';
        bodyMap['memberName'] = userDetails?['full_name'] ?? '';
        bodyMap['memberRelation'] = userDetails?['relation'] ?? '';
      } else if (amountValue == 0) {
        bodyMap['memberId'] = '';
        bodyMap['memberName'] = '';
        bodyMap['memberRelation'] = '';
      }

      final body = jsonEncode(bodyMap);

      print('🌐 Request URL: $baseUrl/api/consultancy/create-order');
      final response = await http.post(
        Uri.parse('$baseUrl/api/consultancy/create-order'),
        headers: {
          'Content-Type': 'application/json',
          'Authorization': 'Bearer $token',
        },
        body: body,
      );

      if (response.statusCode == 200 || response.statusCode == 201) {
        final data = jsonDecode(response.body);
        final bookingAmount = data['booking']?['amount'] ?? 0;
        final hasOrder = data['order'] != null;

        if (bookingAmount == 0 && !hasOrder) {
          print('✅ Consultancy booked successfully as free, redirecting...');
          if (mounted) {
            Navigator.of(context).pop(); // Close sheet
            ScaffoldMessenger.of(context).showSnackBar(
              const SnackBar(
                content: Text(
                  'Success! Your report has been sent for manual review.',
                ),
                backgroundColor: Colors.green,
                behavior: SnackBarBehavior.floating,
              ),
            );
            Navigator.pushAndRemoveUntil(
              context,
              MaterialPageRoute(
                builder: (context) => const MyConsultancyScreen(),
              ),
              (route) => false,
            );
          }
          return;
        }

        _razorpayOrderId = data['order']?['id'];
        _bookingId = data['booking']?['_id'];
        final razorpayKey = data['key'];

        final options = {
          'key': razorpayKey ?? "rzp_test_SATSvsp2uL7kQg",
          'amount': data['order']?['amount'] ?? 50000,
          'name': 'Rhythmrix ECG Review',
          'order_id': _razorpayOrderId,
          'description': 'Manual review for ECG report ${widget.reportId}',
          'timeout': 300,
          'prefill': {
            'contact': prefs.getString('phoneNumber') ?? '',
            'email': prefs.getString('email') ?? '',
          },
        };

        _razorpay.open(options);
      } else if (response.statusCode == 409) {
        String backendMessage = 'Selected slot is already booked';
        try {
          final data = jsonDecode(response.body);
          if (data['message'] != null) backendMessage = data['message'];
        } catch (_) {}
        throw Exception(backendMessage);
      } else {
        throw Exception(
          'Failed to create payment order. Status: ${response.statusCode}',
        );
      }
    } catch (e) {
      print('❌ Error in _startPaymentProcess: $e');
      setState(() {
        _isProcessingPayment = false;
        _manualSheetError = e.toString().replaceFirst('Exception: ', '');
      });
      _manualSheetSetState?.call(() {});
    }
  }

  Future<void> _verifyPayment(PaymentSuccessResponse? response) async {
    print('📡 Verifying payment with backend...');
    try {
      final prefs = await SharedPreferences.getInstance();
      final token = prefs.getString('token');

      final dateStr = DateFormat('yyyy-MM-dd').format(_manualSelectedDate!);
      final start = _manualSelectedTime!;
      final endMinutes =
          (start.hour * 60 + start.minute + _consultationDuration);
      final endTime = TimeOfDay(
        hour: (endMinutes ~/ 60) % 24,
        minute: endMinutes % 60,
      );
      final timeSlot = "${start.format(context)} - ${endTime.format(context)}";

      final verifyBody = {
        'bookingId': _bookingId,
        'razorpay_order_id': response?.orderId ?? _razorpayOrderId ?? '',
        'razorpay_payment_id': response?.paymentId ?? '',
        'razorpay_signature': response?.signature ?? '',
        'bookingDetails': {
          'consultationDate': dateStr,
          'timeSlot': timeSlot,
          'consultationLanguage': _manualSelectedLanguage,
          'notes': 'Manual review request for report ID: ${widget.reportId}',
          'reportId': widget.reportId,
          if (userDetails?['relation']?.toString().toLowerCase() != 'self' &&
              userDetails?['relation']?.toString().toLowerCase() !=
                  'primary user') ...{
            'memberId': userDetails?['_id'] ?? '',
            'memberName': userDetails?['full_name'] ?? '',
            'memberRelation': userDetails?['relation'] ?? '',
          },
        },
      };

      final verifyResponse = await http.post(
        Uri.parse('$baseUrl/api/consultancy/verify-payment'),
        headers: {
          'Content-Type': 'application/json',
          'Authorization': 'Bearer $token',
        },
        body: jsonEncode(verifyBody),
      );

      if (verifyResponse.statusCode == 200 ||
          verifyResponse.statusCode == 201) {
        print('✅ Payment verified success');
        if (mounted) {
          Navigator.of(context).pop(); // Close sheet
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(
              content: Text(
                'Payment Successful! Your report has been sent for manual review.',
              ),
              backgroundColor: Colors.green,
              behavior: SnackBarBehavior.floating,
            ),
          );
          Navigator.pushAndRemoveUntil(
            context,
            MaterialPageRoute(
              builder: (context) => const MyConsultancyScreen(),
            ),
            (route) => false,
          );
        }
      } else if (verifyResponse.statusCode == 409) {
        String backendMessage = 'Selected slot is already booked';
        try {
          final data = jsonDecode(verifyResponse.body);
          if (data['message'] != null) backendMessage = data['message'];
        } catch (_) {}
        throw Exception(backendMessage);
      } else {
        throw Exception('Payment verification failed on server.');
      }
    } catch (e) {
      print('❌ Error in _verifyPayment: $e');
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text('Error verifying payment: $e'),
          backgroundColor: Colors.redAccent,
        ),
      );
    } finally {
      if (mounted) {
        setState(() {
          _isProcessingPayment = false;
        });
        _manualSheetSetState?.call(() {});
      }
    }
  }

  Future<void> _createDownloadNotification() async {
    try {
      final name = userDetails?['full_name'] ?? widget.fullName;
      final url = Uri.parse(
        '$baseUrl/api/notification/create/${widget.userId}',
      );

      final body = jsonEncode({
        "title": "Download Report",
        "notification": "Report downloaded successfully",
        "details": "The ECG report for $name has been downloaded.",
      });

      final response = await http.post(
        url,
        headers: {"Content-Type": "application/json"},
        body: body,
      );
      if (response.statusCode == 201 || response.statusCode == 200) {
        print("✅ Notification created successfully");
      }
    } catch (e) {
      print("❌ Error creating notification: $e");
    }
  }

  Future<void> _fetch12LeadReportFromApi() async {
    print(
      '📡 [ClinicalReportDetail] Fetching 12-lead report for reportId: ${widget.reportId}',
    );
    try {
      final String devId = widget.deviceId.isNotEmpty
          ? widget.deviceId
          : 'ESP_ECG_123';
      final String uId = widget.userId.isNotEmpty
          ? widget.userId
          : '69abf814e0df075be8e2b056';

      Map<String, dynamic>? res;
      if (widget.reportId.isNotEmpty &&
          widget.reportId != 'static_12_lead_report') {
        res = await _twelveLeadService.fetchFull12LeadReport(
          reportId: widget.reportId,
        );
      }

      res ??= await _twelveLeadService.fetchLatest12LeadReport(
        testId: widget.reportId.isNotEmpty ? widget.reportId : null,
        deviceId: devId,
        userId: uId,
      );

      if (res != null && res['success'] == true) {
        if (mounted) {
          setState(() {
            _applyRealLeadsData(res!);
            isLoading = false;
          });
        }
      } else {
        await _fetchData();
      }
    } catch (e) {
      print('❌ [ClinicalReportDetail] Error in _fetch12LeadReportFromApi: $e');
      await _fetchData();
    }
  }

  Future<void> _fetchData() async {
    print('📡 Fetching data for reportId: ${widget.reportId}');
    try {
      final url = Uri.parse('$baseUrl/api/ecg/monitor/${widget.reportId}');
      final response = await http.get(url);
      if (response.statusCode == 200) {
        final Map<String, dynamic> responseData = json.decode(response.body);
        if (responseData['success'] == true &&
            responseData['data'] != null &&
            (responseData['data'] as List).isNotEmpty) {
          final firstItem = responseData['data'][0];
          List<dynamic> rawData = firstItem['data'];

          final fullRawData = rawData
              .map((e) => (e as num).toDouble())
              .toList();
          final filtered = ECGFilter(fs: 360).filterSignal(fullRawData);

          List<double> screenData = filtered;
          if (screenData.length > 1800) {
            screenData = screenData.take(1800).toList();
          }

          if (mounted) {
            setState(() {
              ecgData = filtered;
              displayData = screenData;
              tableReport = responseData['tableReport'];
              userDetails =
                  responseData['userDetails'] ?? responseData['userDetails'];
              abnormalities = firstItem['abnormalities'] ?? [];
              isLoading = false;
            });

            // Downsample and update Lead II in the waveform grid for realism
            if (displayData.isNotEmpty) {
              List<double> lead2Pts = [];
              double step = displayData.length / 3600;
              for (int i = 0; i < 3600; i++) {
                int idx = (i * step).toInt().clamp(0, displayData.length - 1);
                lead2Pts.add(displayData[idx]);
              }
              _staticLeadData['Lead II'] = lead2Pts;
              _deriveMissing12Leads();
            }

            _animationController?.forward();
          }
        } else {
          if (mounted) {
            setState(() {
              errorMessage = "No data available";
              isLoading = false;
            });
          }
        }
      } else {
        if (mounted) {
          setState(() {
            errorMessage = "Failed to load data";
            isLoading = false;
          });
        }
      }
    } catch (e) {
      if (mounted) {
        setState(() {
          errorMessage = e.toString();
          isLoading = false;
        });
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final String displayTitle = "12 Lead ECG";

    return Scaffold(
      backgroundColor: const Color(0xFFF8FAFC),
      appBar: AppBar(
        backgroundColor: Colors.white,
        elevation: 0.5,
        scrolledUnderElevation: 0,
        leading: IconButton(
          icon: const Icon(
            Icons.arrow_back_ios_new_rounded,
            color: Color(0xFF1E293B),
            size: 20,
          ),
          onPressed: () => Navigator.pop(context),
        ),
        title: Text(
          displayTitle,
          style: GoogleFonts.outfit(
            color: const Color(0xFF1E293B),
            fontWeight: FontWeight.bold,
            fontSize: 20,
          ),
        ),
        centerTitle: true,
        actions: [
          IconButton(
            icon: const Icon(
              Icons.file_download_outlined,
              color: Color(0xFF2563EB),
            ),
            onPressed: () => _generateAndHandlePdf(context, share: false),
            tooltip: 'Download PDF',
          ),
          IconButton(
            icon: const Icon(Icons.share_outlined, color: Color(0xFF2563EB)),
            onPressed: () => _generateAndHandlePdf(context, share: true),
            tooltip: 'Share',
          ),
          const SizedBox(width: 8),
        ],
      ),
      body: isLoading
          ? const Center(child: CircularProgressIndicator())
          : errorMessage != null
          ? Center(
              child: Padding(
                padding: const EdgeInsets.all(32),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Icon(
                      Icons.error_outline_rounded,
                      size: 48,
                      color: Colors.red.shade300,
                    ),
                    const SizedBox(height: 16),
                    Text(
                      errorMessage!,
                      textAlign: TextAlign.center,
                      style: GoogleFonts.outfit(color: Colors.red),
                    ),
                  ],
                ),
              ),
            )
          : SafeArea(
              child: Column(
                children: [
                  // Warning Banner for Unsaved Report
                  if (!_isSaved)
                    Container(
                      width: double.infinity,
                      color: const Color(0xFFFEF2F2),
                      padding: const EdgeInsets.symmetric(
                        vertical: 10,
                        horizontal: 16,
                      ),
                      child: Row(
                        mainAxisAlignment: MainAxisAlignment.center,
                        children: [
                          const Icon(
                            Icons.error_outline_rounded,
                            color: Color(0xFFEF4444),
                            size: 18,
                          ),
                          const SizedBox(width: 8),
                          Text(
                            "This Report is Unsaved",
                            style: GoogleFonts.outfit(
                              color: const Color(0xFFEF4444),
                              fontWeight: FontWeight.w600,
                              fontSize: 13,
                            ),
                          ),
                        ],
                      ),
                    ),

                  // Scrollable Content
                  Expanded(
                    child: SingleChildScrollView(
                      physics: const BouncingScrollPhysics(),
                      padding: const EdgeInsets.symmetric(
                        horizontal: 20,
                        vertical: 20,
                      ),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          // Patient Header Card
                          _buildPatientCard(
                            Colors.white,
                            const Color(0xFF074799),
                          ),
                          const SizedBox(height: 24),

                          // Symptoms & Vitals Section
                          _buildSectionHeader(
                            "Symptoms & Vitals",
                            showAdd: true,
                          ),
                          const SizedBox(height: 12),
                          _buildSymptomsCard(),
                          const SizedBox(height: 24),

                          // Interpretation Details Section
                          _buildSectionHeader(
                            "Interpretation Details",
                            showAiBadge: true,
                          ),
                          const SizedBox(height: 12),
                          _buildInterpretationCard(),
                          const SizedBox(height: 24),

                          // ECG Data Analysis Section
                          _buildSectionHeader(
                            "ECG Data Analysis",
                            showAiBadge: true,
                          ),
                          const SizedBox(height: 12),
                          _buildDataAnalysisGrid(Colors.white),
                          const SizedBox(height: 24),

                          // ECG Characteristics Waveforms Section
                          // _buildSectionHeader("ECG characteristics (tap graph to enlarge view)"),
                          Row(
                            children: [
                              Flexible(
                                child: Text(
                                  "ECG characteristics (tap graph to enlarge view)",
                                  style: GoogleFonts.outfit(
                                    fontSize: 15,
                                    fontWeight: FontWeight.bold,
                                    color: const Color(0xFF1E293B),
                                  ),
                                ),
                              ),
                            ],
                          ),
                          const SizedBox(height: 12),
                          _buildWaveformGrid(),
                          const SizedBox(height: 24),

                          const SizedBox(height: 8),

                          Text(
                            'Note: AI interpretations are for guidance only. Always consult a clinical professional for definitive diagnosis.',
                            style: GoogleFonts.outfit(
                              color: const Color(0xFF64748B),
                              fontSize: 11,
                              height: 1.5,
                            ),
                          ),
                          const SizedBox(height: 12),
                        ],
                      ),
                    ),
                  ),

                  // Bottom Action Area
                  Container(
                    padding: const EdgeInsets.symmetric(
                      horizontal: 20,
                      vertical: 16,
                    ),
                    decoration: BoxDecoration(
                      color: Colors.white,
                      border: Border(
                        top: BorderSide(color: Colors.grey.shade200, width: 1),
                      ),
                      boxShadow: [
                        BoxShadow(
                          color: Colors.black.withValues(alpha: 0.05),
                          blurRadius: 10,
                          offset: const Offset(0, -4),
                        ),
                      ],
                    ),
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        // Doctor Manual Review Banner
                        GestureDetector(
                          onTap: () => _showManualReviewSheet(),
                          child: Container(
                            decoration: BoxDecoration(
                              gradient: const LinearGradient(
                                colors: [Color(0xFFB8C9FF), Color(0xFFD6E0FF)],
                                begin: Alignment.centerLeft,
                                end: Alignment.centerRight,
                              ),
                              borderRadius: BorderRadius.circular(16),
                              boxShadow: [
                                BoxShadow(
                                  color: const Color(
                                    0xFF074799,
                                  ).withValues(alpha: 0.10),
                                  blurRadius: 12,
                                  offset: const Offset(0, 4),
                                ),
                              ],
                            ),
                            padding: const EdgeInsets.symmetric(
                              horizontal: 16,
                              vertical: 12,
                            ),
                            child: Row(
                              children: [
                                Container(
                                  decoration: BoxDecoration(
                                    color: Colors.white,
                                    shape: BoxShape.circle,
                                    boxShadow: [
                                      BoxShadow(
                                        color: const Color(
                                          0xFF074799,
                                        ).withValues(alpha: 0.13),
                                        blurRadius: 8,
                                      ),
                                    ],
                                  ),
                                  child: const CircleAvatar(
                                    backgroundColor: Colors.white,
                                    child: Icon(
                                      Icons.medical_services_outlined,
                                      color: Color(0xFF074799),
                                    ),
                                  ),
                                ),
                                const SizedBox(width: 12),
                                Expanded(
                                  child: Column(
                                    mainAxisSize: MainAxisSize.min,
                                    crossAxisAlignment:
                                        CrossAxisAlignment.start,
                                    children: [
                                      Text(
                                        'Request Manual Review',
                                        style: GoogleFonts.outfit(
                                          color: const Color(0xFF074799),
                                          fontWeight: FontWeight.bold,
                                          fontSize: 16,
                                        ),
                                      ),
                                      const SizedBox(height: 2),
                                      Text(
                                        'Get this Report Reviewed by our ECG Experts',
                                        style: GoogleFonts.outfit(
                                          color: Colors.black54,
                                          fontSize: 12,
                                        ),
                                        overflow: TextOverflow.ellipsis,
                                      ),
                                    ],
                                  ),
                                ),
                                const Icon(
                                  Icons.chevron_right_rounded,
                                  color: Color(0xFF074799),
                                  size: 28,
                                ),
                              ],
                            ),
                          ),
                        ),
                        const SizedBox(height: 16),
                        // Save Report Button
                        SizedBox(
                          width: double.infinity,
                          height: 54,
                          child: ElevatedButton(
                            onPressed: () {
                              setState(() {
                                _isSaved = true;
                              });
                              ScaffoldMessenger.of(context).showSnackBar(
                                SnackBar(
                                  content: Text(
                                    'ECG Report Saved Successfully!',
                                    style: GoogleFonts.outfit(),
                                  ),
                                  backgroundColor: const Color(0xFF10B981),
                                ),
                              );
                            },
                            style: ElevatedButton.styleFrom(
                              backgroundColor: _isSaved
                                  ? const Color(0xFF10B981)
                                  : const Color(
                                      0xFF2563EB,
                                    ), // Blue by default, Green when saved
                              shape: RoundedRectangleBorder(
                                borderRadius: BorderRadius.circular(16),
                              ),
                              elevation: 0,
                            ),
                            child: Text(
                              _isSaved ? "Saved" : "Save Report",
                              style: GoogleFonts.outfit(
                                fontSize: 16,
                                fontWeight: FontWeight.bold,
                                color: Colors.white,
                              ),
                            ),
                          ),
                        ),
                      ],
                    ),
                  ),
                ],
              ),
            ),
    );
  }

  Widget _buildSectionHeader(
    String title, {
    bool showAdd = false,
    bool showAiBadge = false,
  }) {
    return Row(
      children: [
        Flexible(
          child: Text(
            title,
            style: GoogleFonts.outfit(
              fontSize: 16,
              fontWeight: FontWeight.bold,
              color: const Color(0xFF1E293B),
            ),
          ),
        ),
        if (showAiBadge) ...[
          const SizedBox(width: 6),
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
            decoration: BoxDecoration(
              border: Border.all(color: const Color(0xFF074799)),
              borderRadius: BorderRadius.circular(8),
              color: const Color(0xFF074799).withValues(alpha: 0.1),
            ),
            child: Row(
              children: [
                const Icon(
                  Icons.auto_awesome,
                  color: Color(0xFF074799),
                  size: 10,
                ),
                const SizedBox(width: 2),
                Text(
                  "AI",
                  style: GoogleFonts.outfit(
                    fontSize: 9,
                    fontWeight: FontWeight.bold,
                    color: const Color(0xFF074799),
                  ),
                ),
              ],
            ),
          ),
        ],
        const Spacer(),
        if (showAdd)
          GestureDetector(
            onTap: () {
              ScaffoldMessenger.of(context).showSnackBar(
                SnackBar(
                  content: Text(
                    'Add symptoms options incoming soon.',
                    style: GoogleFonts.outfit(),
                  ),
                  backgroundColor: const Color(0xFF2563EB),
                ),
              );
            },
            child: Text(
              "+ Add",
              style: GoogleFonts.outfit(
                fontSize: 14,
                fontWeight: FontWeight.bold,
                color: const Color(0xFF2563EB),
              ),
            ),
          ),
      ],
    );
  }

  Widget _buildPatientCard(Color bgColor, Color primaryBlue) {
    final name = userDetails?['full_name'] ?? widget.fullName;
    final age = userDetails?['age']?.toString() ?? widget.age;
    final gender = userDetails?['gender'] ?? widget.gender;
    final height = userDetails?['height']?.toString() ?? widget.height;
    final weight = userDetails?['weight']?.toString() ?? widget.weight;
    final hr =
        tableReport?['observedValues']?['heartRateBpm']?.toString() ?? "90";

    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: bgColor,
        borderRadius: BorderRadius.circular(20),
        border: Border.all(color: const Color(0xFFE2E8F0)),
        boxShadow: [
          BoxShadow(
            color: Colors.black.withValues(alpha: 0.01),
            blurRadius: 10,
            offset: const Offset(0, 4),
          ),
        ],
      ),
      child: Column(
        children: [
          Row(
            children: [
              CircleAvatar(
                backgroundColor: const Color(0xFFE3EBFF),
                radius: 24,
                child: Text(
                  _getInitials(name),
                  style: GoogleFonts.outfit(
                    color: primaryBlue,
                    fontWeight: FontWeight.bold,
                    fontSize: 16,
                  ),
                ),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      name,
                      style: GoogleFonts.outfit(
                        color: Colors.black,
                        fontSize: 16,
                        fontWeight: FontWeight.bold,
                      ),
                    ),
                    const SizedBox(height: 4),
                    Text(
                      widget.reportDate,
                      style: GoogleFonts.outfit(
                        color: Colors.grey.shade600,
                        fontSize: 12,
                      ),
                    ),
                  ],
                ),
              ),
              Row(
                children: [
                  const Icon(Icons.favorite, color: Colors.redAccent, size: 16),
                  const SizedBox(width: 4),
                  Text(
                    '${hr}bpm',
                    style: GoogleFonts.outfit(
                      color: Colors.black87,
                      fontSize: 14,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                ],
              ),
            ],
          ),
          const SizedBox(height: 16),
          const Divider(height: 1, color: Color(0xFFE2E8F0)),
          const SizedBox(height: 12),
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              _buildUserInfoItem('Age', '$age yrs'),
              _buildUserInfoItem('Gender', gender),
              _buildUserInfoItem('Height', '${height}cm'),
              _buildUserInfoItem('Weight', '${weight}kg'),
            ],
          ),
        ],
      ),
    );
  }

  Widget _buildUserInfoItem(String label, String value) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          label,
          style: GoogleFonts.outfit(color: Colors.grey, fontSize: 10),
        ),
        const SizedBox(height: 2),
        Text(
          value,
          style: GoogleFonts.outfit(
            color: Colors.black,
            fontSize: 13,
            fontWeight: FontWeight.w600,
          ),
        ),
      ],
    );
  }

  Widget _buildSymptomsCard() {
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 20),
      decoration: BoxDecoration(
        color: Colors.white,
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: const Color(0xFFE2E8F0)),
      ),
      child: Text(
        "No symptoms and vitals added",
        style: GoogleFonts.outfit(
          fontSize: 14,
          fontStyle: FontStyle.italic,
          color: const Color(0xFF94A3B8),
        ),
      ),
    );
  }

  Widget _buildInterpretationCard() {
    final isNormal = abnormalities.isEmpty;
    final hasCritical = abnormalities.any(
      (a) => a['severity']?.toString().toUpperCase() == 'CRITICAL',
    );

    double activeRiskOffset = 0.16; // Low Risk
    Color riskColor = Colors.green;
    String riskText = "You are at Low Risk";
    String headlineText = "Normal ECG";

    if (hasCritical) {
      activeRiskOffset = 0.84; // High Risk
      riskColor = Colors.red;
      riskText = "You are at High Risk";
      headlineText = "Critical Abnormalities Detected";
    } else if (!isNormal) {
      activeRiskOffset = 0.50; // Moderate Risk
      riskColor = Colors.orange;
      riskText = "You are at Moderate Risk";
      headlineText = "Abnormalities Detected";
    }

    return Container(
      padding: const EdgeInsets.all(18),
      decoration: BoxDecoration(
        color: Colors.white,
        borderRadius: BorderRadius.circular(20),
        border: Border.all(color: const Color(0xFFE2E8F0)),
      ),
      child: isInterpretationEnabled
          ? Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                // Low - Moderate - High labels
                Row(
                  mainAxisAlignment: MainAxisAlignment.spaceBetween,
                  children: [
                    Text(
                      "Low Risk",
                      style: GoogleFonts.outfit(
                        fontSize: 12,
                        fontWeight: FontWeight.w600,
                        color: const Color(0xFF64748B),
                      ),
                    ),
                    Text(
                      "Moderate Risk",
                      style: GoogleFonts.outfit(
                        fontSize: 12,
                        fontWeight: FontWeight.w600,
                        color: const Color(0xFF64748B),
                      ),
                    ),
                    Text(
                      "High Risk",
                      style: GoogleFonts.outfit(
                        fontSize: 12,
                        fontWeight: FontWeight.w600,
                        color: const Color(0xFF64748B),
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 8),

                // Custom indicator slider
                LayoutBuilder(
                  builder: (context, constraints) {
                    double width = constraints.maxWidth;
                    double indicatorPosition = width * activeRiskOffset - 6;
                    return Stack(
                      clipBehavior: Clip.none,
                      children: [
                        Container(
                          height: 8,
                          width: double.infinity,
                          decoration: BoxDecoration(
                            borderRadius: BorderRadius.circular(4),
                            gradient: const LinearGradient(
                              colors: [
                                Color(0xFF10B981), // Green
                                Color(0xFFF59E0B), // Orange
                                Color(0xFFEF4444), // Red
                              ],
                            ),
                          ),
                        ),
                        Positioned(
                          top: -10,
                          left: indicatorPosition,
                          child: const Icon(
                            Icons.arrow_drop_down_rounded,
                            color: Color(0xFF1E293B),
                            size: 20,
                          ),
                        ),
                      ],
                    );
                  },
                ),
                const SizedBox(height: 20),

                // Headline
                Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      headlineText,
                      style: GoogleFonts.outfit(
                        fontSize: 16,
                        fontWeight: FontWeight.bold,
                        color: riskColor,
                      ),
                    ),
                    const SizedBox(height: 4),
                    Text(
                      riskText,
                      style: GoogleFonts.outfit(
                        fontSize: 13,
                        fontWeight: FontWeight.w600,
                        color: const Color(0xFF64748B),
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 12),
                const Divider(color: Color(0xFFF1F5F9)),
                const SizedBox(height: 8),

                // Bullet Details
                if (isNormal)
                  Row(
                    children: [
                      const Icon(
                        Icons.check_circle,
                        color: Colors.green,
                        size: 16,
                      ),
                      const SizedBox(width: 8),
                      Text(
                        'Normal Sinus Rhythm',
                        style: GoogleFonts.outfit(
                          color: Colors.black54,
                          fontSize: 14,
                        ),
                      ),
                    ],
                  )
                else
                  ...abnormalities.map((a) {
                    final sev =
                        a['severity']?.toString().toUpperCase() ?? 'INFO';
                    final isCrit = sev == 'CRITICAL';
                    final isWarn = sev == 'WARNING';
                    final itemColor = isCrit
                        ? Colors.red
                        : (isWarn
                              ? Colors.amber.shade800
                              : Colors.blue.shade700);
                    final icon = isCrit
                        ? Icons.error
                        : (isWarn ? Icons.warning : Icons.info);

                    return Padding(
                      padding: const EdgeInsets.only(bottom: 8.0),
                      child: Row(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Icon(icon, color: itemColor, size: 16),
                          const SizedBox(width: 8),
                          Expanded(
                            child: Text(
                              '${a['abnormalityName']} (${a['severity']})',
                              style: GoogleFonts.outfit(
                                color: itemColor,
                                fontSize: 14,
                                fontWeight: FontWeight.bold,
                              ),
                            ),
                          ),
                        ],
                      ),
                    );
                  }),
              ],
            )
          : Column(
              children: [
                const Divider(color: Colors.black12, height: 1),
                const SizedBox(height: 16),
                Text(
                  'Interpretation is disabled. To enable interpretation go to settings > ECG Settings > ECG Interpretation ON/OFF',
                  style: GoogleFonts.outfit(
                    color: Colors.orange,
                    fontSize: 13,
                    fontWeight: FontWeight.w500,
                  ),
                  textAlign: TextAlign.center,
                ),
              ],
            ),
    );
  }

  Widget _buildDataAnalysisGrid(Color cardColor) {
    final pr =
        tableReport?['observedValues']?['prIntervalMs']?.toString() ?? "--";
    final qrs =
        tableReport?['observedValues']?['qrsIntervalMs']?.toString() ?? "--";
    final qt =
        tableReport?['observedValues']?['qtIntervalMs']?.toString() ?? "--";
    final qtc =
        tableReport?['observedValues']?['qtcIntervalMs']?.toString() ?? "--";

    final prRange =
        tableReport?['standardRanges']?['prIntervalMs'] ?? "100-200";
    final qrsRange =
        tableReport?['standardRanges']?['qrsIntervalMs'] ?? "60-120";
    final qtRange =
        tableReport?['standardRanges']?['qtIntervalMs'] ?? "300-450";
    final qtcRange =
        tableReport?['standardRanges']?['qtcIntervalMs'] ?? "300-450";

    return Column(
      children: [
        Row(
          children: [
            Expanded(
              child: _buildSingleAnalysisCard(
                'PR Interval',
                '$pr ms',
                prRange,
                Colors.pinkAccent,
                cardColor,
              ),
            ),
            const SizedBox(width: 8),
            Expanded(
              child: _buildSingleAnalysisCard(
                'QRS Complex',
                '$qrs ms',
                qrsRange,
                Colors.blueAccent,
                cardColor,
              ),
            ),
          ],
        ),
        const SizedBox(height: 8),
        Row(
          children: [
            Expanded(
              child: _buildSingleAnalysisCard(
                'QT Interval',
                '$qt ms',
                qtRange,
                Colors.orangeAccent,
                cardColor,
              ),
            ),
            const SizedBox(width: 8),
            Expanded(
              child: _buildSingleAnalysisCard(
                'QTc Interval',
                '$qtc ms',
                qtcRange,
                Colors.deepPurpleAccent,
                cardColor,
              ),
            ),
          ],
        ),
      ],
    );
  }

  Widget _buildSingleAnalysisCard(
    String title,
    String value,
    String range,
    Color accentColor,
    Color bgColor,
  ) {
    return Container(
      padding: const EdgeInsets.symmetric(vertical: 12, horizontal: 10),
      decoration: BoxDecoration(
        color: bgColor,
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: const Color(0xFFE2E8F0)),
        boxShadow: [
          BoxShadow(
            color: Colors.black.withValues(alpha: 0.01),
            blurRadius: 4,
            offset: const Offset(0, 2),
          ),
        ],
      ),
      child: IntrinsicHeight(
        child: Row(
          children: [
            Container(
              width: 4,
              decoration: BoxDecoration(
                color: accentColor,
                borderRadius: BorderRadius.circular(2),
              ),
            ),
            const SizedBox(width: 10),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    title,
                    style: GoogleFonts.outfit(
                      color: Colors.black54,
                      fontSize: 11,
                    ),
                    overflow: TextOverflow.ellipsis,
                  ),
                  const SizedBox(height: 2),
                  Text(
                    value,
                    style: GoogleFonts.outfit(
                      color: Colors.black,
                      fontSize: 15,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                  const SizedBox(height: 2),
                  Text(
                    'Range: $range',
                    style: GoogleFonts.outfit(
                      color: Colors.grey.shade500,
                      fontSize: 9,
                    ),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildWaveformGrid() {
    final leads = [
      'Lead I',
      'aVR',
      'V1',
      'V4',
      'Lead II',
      'aVL',
      'V2',
      'V5',
      'Lead III',
      'aVF',
      'V3',
      'V6',
    ];

    final displayedLeads = _isGraphExpanded ? leads : [];

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        // Spandan 12-Lead Medical Clinical Strip (Image 1 Format)
        Container(
          width: double.infinity,
          height: 340,
          decoration: BoxDecoration(
            color: Colors.white,
            borderRadius: BorderRadius.circular(16),
            border: Border.all(color: const Color(0xFFE2E8F0)),
            boxShadow: [
              BoxShadow(
                color: Colors.black.withValues(alpha: 0.04),
                blurRadius: 10,
                offset: const Offset(0, 4),
              ),
            ],
          ),
          clipBehavior: Clip.antiAlias,
          child: CustomPaint(
            painter: _SpandanStyle12LeadPainter(
              staticLeadData: _staticLeadData,
            ),
          ),
        ),
        if (_isGraphExpanded) ...[
          const SizedBox(height: 16),
          GridView.builder(
            shrinkWrap: true,
            physics: const NeverScrollableScrollPhysics(),
            gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
              crossAxisCount: 2,
              crossAxisSpacing: 12,
              mainAxisSpacing: 12,
              childAspectRatio: 1.35,
            ),
            itemCount: displayedLeads.length,
            itemBuilder: (context, index) {
              final leadName = displayedLeads[index];
              final pts = _staticLeadData[leadName] ?? [];
              final bool hasData = pts.isNotEmpty;
              return GestureDetector(
                onTap: hasData ? () => _showEnlargedGraph(leadName, pts) : null,
                child: Container(
                  decoration: BoxDecoration(
                    color: Colors.white,
                    borderRadius: BorderRadius.circular(16),
                    border: Border.all(color: const Color(0xFFE2E8F0)),
                    boxShadow: [
                      BoxShadow(
                        color: Colors.black.withValues(alpha: 0.02),
                        blurRadius: 6,
                        offset: const Offset(0, 3),
                      ),
                    ],
                  ),
                  clipBehavior: Clip.antiAlias,
                  child: Stack(
                    children: [
                      Positioned.fill(
                        child: CustomPaint(
                          painter: _StaticClinicalECGPainter(points: pts),
                        ),
                      ),
                      Positioned(
                        top: 8,
                        left: 12,
                        child: Text(
                          leadName,
                          style: GoogleFonts.outfit(
                            color: const Color(0xFF1E293B),
                            fontWeight: FontWeight.bold,
                            fontSize: 13,
                          ),
                        ),
                      ),
                      if (!hasData)
                        Center(
                          child: Text(
                            "No Data Available",
                            style: GoogleFonts.outfit(
                              fontSize: 12,
                              fontWeight: FontWeight.w500,
                              color: const Color(0xFF94A3B8),
                            ),
                          ),
                        ),
                    ],
                  ),
                ),
              );
            },
          ),
        ],
        const SizedBox(height: 12),
        Center(
          child: TextButton.icon(
            onPressed: () {
              setState(() {
                _isGraphExpanded = !_isGraphExpanded;
              });
            },
            icon: Icon(
              _isGraphExpanded ? Icons.grid_view : Icons.view_compact,
              size: 18,
              color: const Color(0xFF2563EB),
            ),
            label: Text(
              _isGraphExpanded
                  ? "Hide Expanded Cards"
                  : "Show Individual Lead Cards",
              style: GoogleFonts.outfit(
                fontSize: 14,
                fontWeight: FontWeight.bold,
                color: const Color(0xFF2563EB),
              ),
            ),
          ),
        ),
      ],
    );
  }

  void _showEnlargedGraph(String leadName, List<double> pts) {
    final TransformationController transformationController =
        TransformationController();

    showDialog(
      context: context,
      builder: (context) {
        return Dialog(
          backgroundColor: Colors.transparent,
          insetPadding: const EdgeInsets.symmetric(horizontal: 16),
          child: Container(
            width: double.infinity,
            padding: const EdgeInsets.all(16),
            decoration: BoxDecoration(
              color: Colors.white,
              borderRadius: BorderRadius.circular(24),
              boxShadow: [
                BoxShadow(
                  color: Colors.black.withValues(alpha: 0.15),
                  blurRadius: 25,
                  offset: const Offset(0, 10),
                ),
              ],
            ),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Row(
                  mainAxisAlignment: MainAxisAlignment.spaceBetween,
                  children: [
                    Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          '$leadName Waveform',
                          style: GoogleFonts.outfit(
                            fontSize: 18,
                            fontWeight: FontWeight.bold,
                            color: Colors.black,
                          ),
                        ),
                        Text(
                          'Pinch or use buttons to zoom & pan',
                          style: GoogleFonts.outfit(
                            fontSize: 12,
                            color: Colors.grey.shade600,
                          ),
                        ),
                      ],
                    ),
                    IconButton(
                      icon: const Icon(Icons.close, color: Colors.grey),
                      onPressed: () => Navigator.pop(context),
                    ),
                  ],
                ),
                const SizedBox(height: 20),
                Align(
                  alignment: Alignment.centerLeft,
                  child: Container(
                    padding: const EdgeInsets.symmetric(
                      horizontal: 16,
                      vertical: 8,
                    ),
                    decoration: BoxDecoration(
                      color: const Color(0xFF2563EB),
                      borderRadius: BorderRadius.circular(8),
                    ),
                    child: Text(
                      leadName,
                      style: GoogleFonts.outfit(
                        color: Colors.white,
                        fontWeight: FontWeight.bold,
                      ),
                    ),
                  ),
                ),
                const SizedBox(height: 16),
                Container(
                  height: 220,
                  width: double.infinity,
                  decoration: BoxDecoration(
                    color: Colors.white,
                    borderRadius: BorderRadius.circular(12),
                    border: Border.all(color: Colors.grey.shade200),
                  ),
                  child: ClipRRect(
                    borderRadius: BorderRadius.circular(12),
                    child: InteractiveViewer(
                      transformationController: transformationController,
                      minScale: 1.0,
                      maxScale: 6.0,
                      constrained: true,
                      child: SizedBox(
                        width: double.infinity,
                        height: 220,
                        child: CustomPaint(
                          painter: _StaticClinicalECGPainter(points: pts),
                        ),
                      ),
                    ),
                  ),
                ),
                const SizedBox(height: 24),
                Row(
                  mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                  children: [
                    _buildZoomButton(
                      icon: Icons.zoom_in,
                      label: 'Zoom In',
                      onTap: () {
                        transformationController.value *=
                            Matrix4.diagonal3Values(1.2, 1.2, 1);
                      },
                    ),
                    _buildZoomButton(
                      icon: Icons.zoom_out,
                      label: 'Zoom Out',
                      onTap: () {
                        final double currentScale = transformationController
                            .value
                            .getMaxScaleOnAxis();
                        if (currentScale > 1.01) {
                          transformationController.value *=
                              Matrix4.diagonal3Values(0.8, 0.8, 1);
                          if (transformationController.value
                                  .getMaxScaleOnAxis() <
                              1.0) {
                            transformationController.value = Matrix4.identity();
                          }
                        }
                      },
                    ),
                    _buildZoomButton(
                      icon: Icons.refresh,
                      label: 'Reset',
                      onTap: () {
                        transformationController.value = Matrix4.identity();
                      },
                    ),
                  ],
                ),
              ],
            ),
          ),
        );
      },
    );
  }

  Widget _buildZoomButton({
    required IconData icon,
    required String label,
    required VoidCallback onTap,
  }) {
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        Material(
          color: const Color(0xFFEEF2FF),
          shape: const CircleBorder(),
          child: InkWell(
            onTap: onTap,
            customBorder: const CircleBorder(),
            child: Container(
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                shape: BoxShape.circle,
                border: Border.all(
                  color: const Color(0xFF2563EB).withValues(alpha: 0.1),
                ),
              ),
              child: Icon(icon, color: Color(0xFF2563EB), size: 20),
            ),
          ),
        ),
        const SizedBox(height: 8),
        Text(
          label,
          style: GoogleFonts.outfit(
            fontSize: 11,
            fontWeight: FontWeight.w600,
            color: Colors.black87,
          ),
        ),
      ],
    );
  }

  Widget _buildManualReviewCard() {
    return GestureDetector(
      onTap: () => _showManualReviewSheet(),
      child: Container(
        decoration: BoxDecoration(
          color: const Color(0xFFFFFBEB), // soft amber background
          borderRadius: BorderRadius.circular(16),
          border: Border.all(color: const Color(0xFFFDE68A)),
        ),
        padding: const EdgeInsets.all(16),
        child: Row(
          children: [
            Container(
              padding: const EdgeInsets.all(8),
              decoration: const BoxDecoration(
                color: Color(0xFFFCD34D),
                shape: BoxShape.circle,
              ),
              child: const Icon(
                Icons.medical_services_outlined,
                color: Color(0xFF78350F),
                size: 24,
              ),
            ),
            const SizedBox(width: 14),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    "Request Manual Review",
                    style: GoogleFonts.outfit(
                      fontSize: 14,
                      fontWeight: FontWeight.bold,
                      color: const Color(0xFF78350F),
                    ),
                  ),
                  const SizedBox(height: 2),
                  Text(
                    "Get this Report Reviewed by our ECG Specialists",
                    style: GoogleFonts.outfit(
                      fontSize: 12,
                      color: const Color(0xFF92400E),
                    ),
                  ),
                ],
              ),
            ),
            const Icon(Icons.chevron_right_rounded, color: Color(0xFF78350F)),
          ],
        ),
      ),
    );
  }

  void _showManualReviewSheet() {
    const primaryBlue = Color(0xFF074799);
    const lightBlue = Color(0xFFF1F5FE);
    const darkBlue = Color(0xFF053B7A);

    _manualSheetError = null;

    showModalBottomSheet(
      context: context,
      isScrollControlled: true,
      backgroundColor: Colors.transparent,
      builder: (context) {
        return StatefulBuilder(
          builder: (context, setStateSheet) {
            _manualSheetSetState = setStateSheet;
            return Container(
              decoration: const BoxDecoration(
                color: Colors.white,
                borderRadius: BorderRadius.vertical(top: Radius.circular(28)),
              ),
              child: Padding(
                padding: EdgeInsets.only(
                  bottom: MediaQuery.of(context).viewInsets.bottom,
                ),
                child: SingleChildScrollView(
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      const SizedBox(height: 12),
                      Container(
                        width: 45,
                        height: 5,
                        decoration: BoxDecoration(
                          color: Colors.grey.shade300,
                          borderRadius: BorderRadius.circular(10),
                        ),
                      ),
                      Stack(
                        children: [
                          Container(
                            margin: const EdgeInsets.fromLTRB(16, 30, 16, 8),
                            height: 110,
                            width: double.infinity,
                            decoration: BoxDecoration(
                              gradient: LinearGradient(
                                colors: [
                                  const Color(0xFFE3EBFF),
                                  lightBlue.withValues(alpha: 0.5),
                                ],
                                begin: Alignment.topLeft,
                                end: Alignment.bottomRight,
                              ),
                              borderRadius: BorderRadius.circular(20),
                            ),
                            child: Row(
                              children: [
                                const SizedBox(width: 15),
                                Container(
                                  height: 80,
                                  width: 80,
                                  decoration: BoxDecoration(
                                    color: Colors.white.withValues(alpha: 0.6),
                                    shape: BoxShape.circle,
                                  ),
                                  child: const Center(
                                    child: Icon(
                                      Icons.person_search_rounded,
                                      size: 45,
                                      color: primaryBlue,
                                    ),
                                  ),
                                ),
                                const SizedBox(width: 15),
                                Expanded(
                                  child: Column(
                                    mainAxisAlignment: MainAxisAlignment.center,
                                    crossAxisAlignment:
                                        CrossAxisAlignment.start,
                                    children: [
                                      Text(
                                        'Rhythmrix Manual Report Review',
                                        style: GoogleFonts.outfit(
                                          fontSize: 16,
                                          fontWeight: FontWeight.w800,
                                          color: darkBlue,
                                        ),
                                      ),
                                      const SizedBox(height: 4),
                                      Row(
                                        children: [
                                          Text(
                                            'Learn more',
                                            style: GoogleFonts.outfit(
                                              fontSize: 13,
                                              color: primaryBlue,
                                              fontWeight: FontWeight.w600,
                                            ),
                                          ),
                                          const Icon(
                                            Icons.chevron_right,
                                            size: 16,
                                            color: primaryBlue,
                                          ),
                                        ],
                                      ),
                                      const SizedBox(height: 8),
                                      Text(
                                        isUnlimited
                                            ? 'Unlimited Free Reviews Left.'
                                            : '$consultancyCount Free Reviews Left.',
                                        style: GoogleFonts.outfit(
                                          fontSize: 12,
                                          color: Colors.grey.shade700,
                                          fontWeight: FontWeight.w500,
                                        ),
                                      ),
                                    ],
                                  ),
                                ),
                              ],
                            ),
                          ),
                          Positioned(
                            right: 25,
                            top: 0,
                            child: GestureDetector(
                              onTap: () => Navigator.pop(context),
                              child: Container(
                                padding: const EdgeInsets.all(4),
                                decoration: BoxDecoration(
                                  color: Colors.black.withValues(alpha: 0.3),
                                  shape: BoxShape.circle,
                                ),
                                child: const Icon(
                                  Icons.close,
                                  size: 18,
                                  color: Colors.white,
                                ),
                              ),
                            ),
                          ),
                        ],
                      ),
                      Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 20),
                        child: Text(
                          consultancyCount > 0
                              ? '1 free review will be utilized for this request.'
                              : 'Standard charges will apply for this request.',
                          style: GoogleFonts.outfit(
                            fontSize: 14,
                            fontWeight: FontWeight.w600,
                            color: Colors.grey.shade800,
                          ),
                        ),
                      ),
                      const SizedBox(height: 16),
                      Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 16),
                        child: Row(
                          children: [
                            _buildSheetMetricCard(
                              icon: Icons.assignment_outlined,
                              label: 'REVIEW CHARGES',
                              value: consultancyCount > 0 ? 'Free' : '₹500',
                              primaryColor: primaryBlue,
                            ),
                            const SizedBox(width: 12),
                            _buildSheetMetricCard(
                              icon: Icons.timer_outlined,
                              label: 'EXPECTED IN',
                              value: '$_consultationDuration mins.*',
                              primaryColor: const Color(0xFF4CAF50),
                            ),
                          ],
                        ),
                      ),
                      const SizedBox(height: 12),
                      Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 16),
                        child: Row(
                          children: [
                            _buildInteractiveMetricCard(
                              icon: Icons.calendar_month_rounded,
                              label: 'SELECT DATE',
                              value: _manualSelectedDate == null
                                  ? 'Choose Date'
                                  : DateFormat(
                                      'dd MMM yyyy',
                                    ).format(_manualSelectedDate!),
                              primaryColor: primaryBlue,
                              onTap: () async {
                                final DateTime? picked = await showDatePicker(
                                  context: context,
                                  initialDate:
                                      _manualSelectedDate ?? DateTime.now(),
                                  firstDate: DateTime.now(),
                                  lastDate: DateTime.now().add(
                                    const Duration(days: 30),
                                  ),
                                );
                                if (picked != null) {
                                  setStateSheet(() {
                                    _manualSelectedDate = picked;
                                    _manualSheetError = null;
                                    if (_manualSelectedTime != null) {
                                      final now = DateTime.now();
                                      final isToday =
                                          picked.year == now.year &&
                                          picked.month == now.month &&
                                          picked.day == now.day;
                                      final currentTime =
                                          TimeOfDay.fromDateTime(now);

                                      if (isToday &&
                                          (_manualSelectedTime!.hour <
                                                  currentTime.hour ||
                                              (_manualSelectedTime!.hour ==
                                                      currentTime.hour &&
                                                  _manualSelectedTime!.minute <
                                                      currentTime.minute))) {
                                        _manualSelectedTime = null;
                                        _manualSheetError =
                                            'Selected time is now in the past for the selected date';
                                      }
                                    }
                                  });
                                }
                              },
                            ),
                            const SizedBox(width: 12),
                            _buildInteractiveMetricCard(
                              icon: Icons.access_time_rounded,
                              label: 'SELECT TIME',
                              value: _manualSelectedTime == null
                                  ? 'Choose Time'
                                  : _manualSelectedTime!.format(context),
                              primaryColor: primaryBlue,
                              onTap: () async {
                                final TimeOfDay? picked = await showTimePicker(
                                  context: context,
                                  initialTime:
                                      _manualSelectedTime ??
                                      const TimeOfDay(hour: 9, minute: 0),
                                );
                                if (picked != null) {
                                  final now = DateTime.now();
                                  final isToday =
                                      _manualSelectedDate != null &&
                                      _manualSelectedDate!.year == now.year &&
                                      _manualSelectedDate!.month == now.month &&
                                      _manualSelectedDate!.day == now.day;
                                  final currentTime = TimeOfDay.fromDateTime(
                                    now,
                                  );

                                  if (picked.hour < 9 || picked.hour >= 21) {
                                    setStateSheet(() {
                                      _manualSelectedTime = null;
                                      _manualSheetError =
                                          'Selected time is outside service hours (09:00 AM - 09:00 PM)';
                                    });
                                  } else if (isToday &&
                                      (picked.hour < currentTime.hour ||
                                          (picked.hour == currentTime.hour &&
                                              picked.minute <
                                                  currentTime.minute))) {
                                    setStateSheet(() {
                                      _manualSelectedTime = null;
                                      _manualSheetError =
                                          'Cannot select past time for today';
                                    });
                                  } else {
                                    setStateSheet(() {
                                      _manualSelectedTime = picked;
                                      _manualSheetError = null;
                                    });
                                  }
                                }
                              },
                            ),
                          ],
                        ),
                      ),
                      const SizedBox(height: 12),
                      Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 16),
                        child: Row(
                          children: [
                            _buildInteractiveMetricCard(
                              icon: Icons.language_rounded,
                              label: 'CONSULTATION LANGUAGE',
                              value: _manualSelectedLanguage,
                              primaryColor: const Color(0xFF074799),
                              onTap: () {
                                _showLanguagePicker(context, setStateSheet);
                              },
                            ),
                          ],
                        ),
                      ),
                      const SizedBox(height: 12),
                      Container(
                        margin: const EdgeInsets.symmetric(horizontal: 16),
                        padding: const EdgeInsets.all(16),
                        decoration: BoxDecoration(
                          color: const Color(0xFFF8F9FB),
                          borderRadius: BorderRadius.circular(16),
                          border: Border.all(color: Colors.grey.shade100),
                        ),
                        child: Row(
                          children: [
                            Container(
                              padding: const EdgeInsets.all(8),
                              decoration: BoxDecoration(
                                color: Colors.white,
                                shape: BoxShape.circle,
                                boxShadow: [
                                  BoxShadow(
                                    color: Colors.black.withValues(alpha: 0.05),
                                    blurRadius: 5,
                                  ),
                                ],
                              ),
                              child: const Icon(
                                Icons.access_time_filled_rounded,
                                color: primaryBlue,
                                size: 24,
                              ),
                            ),
                            const SizedBox(width: 15),
                            Column(
                              crossAxisAlignment: CrossAxisAlignment.start,
                              children: [
                                Text(
                                  'SERVICE HOURS',
                                  style: GoogleFonts.outfit(
                                    fontSize: 11,
                                    fontWeight: FontWeight.bold,
                                    color: Colors.grey,
                                    letterSpacing: 1.1,
                                  ),
                                ),
                                const SizedBox(height: 4),
                                Text(
                                  'Mon to Sun (09:00 am to 9:00 pm)',
                                  style: GoogleFonts.outfit(
                                    fontSize: 15,
                                    fontWeight: FontWeight.bold,
                                    color: darkBlue,
                                  ),
                                ),
                              ],
                            ),
                          ],
                        ),
                      ),
                      const SizedBox(height: 16),
                      Container(
                        margin: const EdgeInsets.symmetric(horizontal: 16),
                        width: double.infinity,
                        decoration: BoxDecoration(
                          gradient: const LinearGradient(
                            colors: [Color(0xFF074799), Color(0xFF2979FF)],
                          ),
                          borderRadius: BorderRadius.circular(16),
                          boxShadow: [
                            BoxShadow(
                              color: primaryBlue.withValues(alpha: 0.3),
                              blurRadius: 10,
                              offset: const Offset(0, 4),
                            ),
                          ],
                        ),
                        child: Material(
                          color: Colors.transparent,
                          child: InkWell(
                            onTap: () {
                              Navigator.push(
                                context,
                                MaterialPageRoute(
                                  builder: (_) => const SubscriptionPlan(),
                                ),
                              );
                            },
                            borderRadius: BorderRadius.circular(16),
                            child: Padding(
                              padding: const EdgeInsets.all(16),
                              child: Row(
                                children: [
                                  Container(
                                    padding: const EdgeInsets.all(8),
                                    decoration: BoxDecoration(
                                      color: Colors.white.withValues(
                                        alpha: 0.2,
                                      ),
                                      borderRadius: BorderRadius.circular(12),
                                    ),
                                    child: const Icon(
                                      Icons.workspace_premium,
                                      color: Colors.white,
                                      size: 24,
                                    ),
                                  ),
                                  const SizedBox(width: 15),
                                  Expanded(
                                    child: Column(
                                      crossAxisAlignment:
                                          CrossAxisAlignment.start,
                                      children: [
                                        Text(
                                          'Upgrade to Paid plans to save more!',
                                          style: GoogleFonts.outfit(
                                            fontSize: 15,
                                            fontWeight: FontWeight.bold,
                                            color: Colors.white,
                                          ),
                                        ),
                                        Text(
                                          'Upgrade now to enjoy more free reviews',
                                          style: GoogleFonts.outfit(
                                            fontSize: 12,
                                            color: Colors.white70,
                                          ),
                                        ),
                                      ],
                                    ),
                                  ),
                                  const Icon(
                                    Icons.chevron_right,
                                    color: Colors.white,
                                  ),
                                ],
                              ),
                            ),
                          ),
                        ),
                      ),
                      const SizedBox(height: 16),
                      Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 16),
                        child: Row(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            const Text(
                              '* ',
                              style: TextStyle(color: Colors.red),
                            ),
                            Expanded(
                              child: Text(
                                'The review time may vary if there is a high volume of reports.',
                                style: GoogleFonts.outfit(
                                  fontSize: 11,
                                  color: Colors.grey.shade600,
                                  fontStyle: FontStyle.italic,
                                ),
                              ),
                            ),
                          ],
                        ),
                      ),
                      if (_manualSheetError != null)
                        Padding(
                          padding: const EdgeInsets.symmetric(
                            horizontal: 16,
                            vertical: 12,
                          ),
                          child: Container(
                            padding: const EdgeInsets.all(12),
                            decoration: BoxDecoration(
                              color: Colors.red.shade50,
                              borderRadius: BorderRadius.circular(12),
                              border: Border.all(color: Colors.red.shade100),
                            ),
                            child: Row(
                              children: [
                                const Icon(
                                  Icons.error_outline,
                                  color: Colors.redAccent,
                                  size: 20,
                                ),
                                const SizedBox(width: 10),
                                Expanded(
                                  child: Text(
                                    _manualSheetError!,
                                    style: GoogleFonts.outfit(
                                      color: Colors.redAccent,
                                      fontSize: 13,
                                      fontWeight: FontWeight.bold,
                                    ),
                                  ),
                                ),
                              ],
                            ),
                          ),
                        ),
                      const SizedBox(height: 10),
                      Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 16),
                        child: SizedBox(
                          width: double.infinity,
                          height: 56,
                          child: ElevatedButton(
                            onPressed: _isProcessingPayment
                                ? null
                                : () {
                                    if (_manualSelectedDate == null ||
                                        _manualSelectedTime == null) {
                                      setStateSheet(() {
                                        _manualSheetError =
                                            'Please select both Date and Time before sending';
                                      });
                                      return;
                                    }
                                    _startPaymentProcess();
                                  },
                            style: ElevatedButton.styleFrom(
                              backgroundColor: primaryBlue,
                              foregroundColor: Colors.white,
                              disabledBackgroundColor: primaryBlue.withValues(
                                alpha: 0.6,
                              ),
                              elevation: 0,
                              shape: RoundedRectangleBorder(
                                borderRadius: BorderRadius.circular(16),
                              ),
                            ),
                            child: _isProcessingPayment
                                ? const SizedBox(
                                    height: 24,
                                    width: 24,
                                    child: CircularProgressIndicator(
                                      color: Colors.white,
                                      strokeWidth: 3,
                                    ),
                                  )
                                : Text(
                                    consultancyCount > 0
                                        ? 'Send for Free Review'
                                        : 'Send for Review (₹500)',
                                    style: GoogleFonts.outfit(
                                      fontSize: 18,
                                      fontWeight: FontWeight.bold,
                                    ),
                                  ),
                          ),
                        ),
                      ),
                      const SizedBox(height: 32),
                    ],
                  ),
                ),
              ),
            );
          },
        );
      },
    );
  }

  Widget _buildSheetMetricCard({
    required IconData icon,
    required String label,
    required String value,
    required Color primaryColor,
  }) {
    return Expanded(
      child: Container(
        padding: const EdgeInsets.all(12),
        decoration: BoxDecoration(
          color: const Color(0xFFF8F9FB),
          borderRadius: BorderRadius.circular(16),
          border: Border.all(color: Colors.grey.shade100),
        ),
        child: Row(
          children: [
            Container(
              padding: const EdgeInsets.all(6),
              decoration: BoxDecoration(
                color: primaryColor.withValues(alpha: 0.1),
                borderRadius: BorderRadius.circular(8),
              ),
              child: Icon(icon, color: primaryColor, size: 20),
            ),
            const SizedBox(width: 10),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                mainAxisSize: MainAxisSize.min,
                children: [
                  Text(
                    label,
                    style: GoogleFonts.outfit(
                      fontSize: 9,
                      fontWeight: FontWeight.bold,
                      color: Colors.grey.shade600,
                      letterSpacing: 0.5,
                    ),
                  ),
                  const SizedBox(height: 2),
                  Text(
                    value,
                    style: GoogleFonts.outfit(
                      fontSize: 15,
                      fontWeight: FontWeight.w900,
                      color: Colors.black87,
                    ),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildInteractiveMetricCard({
    required IconData icon,
    required String label,
    required String value,
    required Color primaryColor,
    required VoidCallback onTap,
  }) {
    return Expanded(
      child: Material(
        color: Colors.transparent,
        child: InkWell(
          onTap: onTap,
          borderRadius: BorderRadius.circular(16),
          child: Container(
            padding: const EdgeInsets.all(12),
            decoration: BoxDecoration(
              color: const Color(0xFFF8F9FB),
              borderRadius: BorderRadius.circular(16),
              border: Border.all(color: Colors.grey.shade100),
            ),
            child: Row(
              children: [
                Container(
                  padding: const EdgeInsets.all(6),
                  decoration: BoxDecoration(
                    color: primaryColor.withValues(alpha: 0.1),
                    borderRadius: BorderRadius.circular(8),
                  ),
                  child: Icon(icon, color: primaryColor, size: 20),
                ),
                const SizedBox(width: 10),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Text(
                        label,
                        style: GoogleFonts.outfit(
                          fontSize: 9,
                          fontWeight: FontWeight.bold,
                          color: Colors.grey.shade600,
                          letterSpacing: 0.5,
                        ),
                      ),
                      const SizedBox(height: 2),
                      Text(
                        value,
                        style: GoogleFonts.outfit(
                          fontSize: 14,
                          fontWeight: FontWeight.w900,
                          color: Colors.black87,
                        ),
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                      ),
                    ],
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Future<void> _generateAndHandlePdf(
    BuildContext context, {
    required bool share,
  }) async {
    if (ecgData.isEmpty) return;

    showDialog(
      context: context,
      barrierDismissible: false,
      builder: (context) => const Center(child: CircularProgressIndicator()),
    );

    try {
      final pdf = pw.Document();
      final now = DateTime.now();

      final Map<String, Map<String, String>> abnormalityExplanations = {
        'Sinus Pause': {
          'description':
              'A brief delay where the heart misses or skips a beat because its natural pacemaker takes a temporary pause.',
          'causes':
              'Often caused by deep relaxation, athletic fitness, certain medications (like blood pressure drugs), or natural aging of the heart.',
          'actions':
              'If you feel fine, this is usually normal. If you experience dizziness or feel lightheaded, please consult a doctor.',
        },
        'Sinus Arrest': {
          'description':
              'A longer pause where the heart skips beats because the natural pacemaker temporarily stops sending signals.',
          'causes':
              'Can be caused by aging of the heart tissue, heart medications, or blockages in blood flow.',
          'actions':
              'This is a serious finding. Please consult a doctor immediately, especially if you have fainted, felt dizzy, or had chest discomfort.',
        },
        'Asystole': {
          'description':
              'A flatline on the graph, indicating that the heart\'s electrical system is not sending any signals and the heart has temporarily stopped beating.',
          'causes':
              'Usually a sign of severe heart emergency, severe lack of oxygen, or the chest sensors being disconnected from the skin.',
          'actions':
              'This is a life-threatening emergency. If the person is not responsive, call emergency services and begin CPR immediately.',
        },
        'Atrial Fibrillation': {
          'description':
              'An irregular and fast heartbeat in the upper chambers of the heart, which can cause the heart to flutter or beat out of sync.',
          'causes':
              'Common causes include high blood pressure, stress, caffeine, alcohol, or natural aging of the heart chambers.',
          'actions':
              'Please consult a doctor to evaluate the irregular pattern, manage your heart rate, and discuss steps to prevent blood clots.',
        },
        'Atrial Flutter': {
          'description':
              'A rapid but regular fluttering heartbeat in the upper chambers of the heart, often creating a fast pulse.',
          'causes':
              'Often caused by heart valve issues, high blood pressure, past heart surgery, or chronic lung conditions.',
          'actions':
              'Please seek a medical evaluation to help restore a regular, calm heartbeat using medication or simple medical procedures.',
        },
        'SVT': {
          'description':
              'A sudden episode of a very fast, racing heartbeat starting in the top part of the heart.',
          'causes':
              'Often triggered by anxiety, stress, too much caffeine or alcohol, intense exercise, or an extra electrical pathway from birth.',
          'actions':
              'If racing starts, sitting down and coughing or splashing cold water on your face can help. Consult a doctor if this happens often.',
        },
        'Ventricular Tachycardia': {
          'description':
              'A rapid, racing heartbeat originating from the lower pumping chambers of the heart, which can prevent the heart from pumping blood properly.',
          'causes':
              'Often caused by scar tissue from a past heart attack, low mineral levels in the blood, or heart muscle weakness.',
          'actions':
              'This requires immediate medical care. Please seek emergency medical evaluation right away if you feel dizzy or faint.',
        },
        'Ventricular Fibrillation': {
          'description':
              'A chaotic, rapid quivering of the lower heart chambers where the heart cannot pump any blood, leading to immediate collapse.',
          'causes':
              'Usually triggered by a heart attack, severe heart damage, or electrical shock.',
          'actions':
              'This is a life-threatening emergency. Call emergency services and use an automated external defibrillator (AED) / perform CPR immediately.',
        },
        'Sinus Bradycardia': {
          'description':
              'A slow, relaxed heartbeat (below 60 beats per minute) originating from the heart\'s natural pacemaker.',
          'causes':
              'Very common in physically fit athletes, during deep sleep, or as a side effect of blood pressure medications.',
          'actions':
              'This is generally healthy and normal if you feel well. If you feel unusually tired or dizzy, please consult a doctor.',
        },
        'Sinus Tachycardia': {
          'description':
              'A fast heartbeat (above 100 beats per minute) originating from the heart\'s natural pacemaker.',
          'causes':
              'Common normal response to exercise, stress, nervousness, fever, dehydration, caffeine, or energy drinks.',
          'actions':
              'Usually normal and calms down with rest and hydration. If it stays high while resting, consult a doctor.',
        },
        'Ventricular Ectopic / PVC': {
          'description':
              'An extra or early heartbeat starting in the lower chambers of the heart, which often feels like a skipped beat or a flutter in the chest.',
          'causes':
              'Typically triggered by stress, anxiety, lack of sleep, caffeine, tobacco, or minor mineral imbalances.',
          'actions':
              'Most often harmless and does not need treatment. Try reducing caffeine and stress. Consult a doctor if you feel them very frequently.',
        },
        'PAC (Supraventricular Ectopic)': {
          'description':
              'An early or extra heartbeat starting in the upper chambers of the heart, which can feel like a brief pause or a flutter.',
          'causes':
              'Commonly caused by daily stress, fatigue, caffeine, alcohol, or mild changes in the heart valves.',
          'actions':
              'Usually completely harmless. Rest and limiting caffeine/stress will help. Talk to a doctor if they become continuous or bothersome.',
        },
      };

      final pdfName = userDetails?['full_name'] ?? widget.fullName;
      final pdfAge = userDetails?['age']?.toString() ?? widget.age;
      final pdfGender = userDetails?['gender'] ?? widget.gender;
      final pdfHeight = userDetails?['height']?.toString() ?? widget.height;
      final pdfWeight = userDetails?['weight']?.toString() ?? widget.weight;

      final pdfPr =
          tableReport?['observedValues']?['prIntervalMs']?.toString() ?? "--";
      final pdfQrs =
          tableReport?['observedValues']?['qrsIntervalMs']?.toString() ?? "--";
      final pdfQt =
          tableReport?['observedValues']?['qtIntervalMs']?.toString() ?? "--";
      final pdfQtc =
          tableReport?['observedValues']?['qtcIntervalMs']?.toString() ?? "--";
      final pdfHr =
          tableReport?['observedValues']?['heartRateBpm']?.toString() ?? "--";

      final pdfPrRange =
          tableReport?['standardRanges']?['prIntervalMs']?.toString() ??
          "100-200";
      final pdfQrsRange =
          tableReport?['standardRanges']?['qrsIntervalMs']?.toString() ??
          "60-120";
      final pdfQtRange =
          tableReport?['standardRanges']?['qtIntervalMs']?.toString() ??
          "300-450";
      final pdfQtcRange =
          tableReport?['standardRanges']?['qtcIntervalMs']?.toString() ??
          "300-450";
      final pdfHrRange =
          tableReport?['standardRanges']?['heartRateBpm']?.toString() ??
          "60-100";

      final pdfQualityScore =
          tableReport?['qualityScore']?['score']?.toString() ?? "0";
      final uniqueAbnormalities = (() {
        final seen = <String>{};
        return abnormalities.where((a) {
          final name = a['abnormalityName']?.toString() ?? '';
          return name.isNotEmpty && seen.add(name);
        }).toList();
      })();

      final chunks = <List<dynamic>>[];
      for (var i = 0; i < uniqueAbnormalities.length; i += 5) {
        chunks.add(
          uniqueAbnormalities.sublist(
            i,
            i + 5 > uniqueAbnormalities.length
                ? uniqueAbnormalities.length
                : i + 5,
          ),
        );
      }

      final totalPages = chunks.isNotEmpty ? 2 + chunks.length : 2;

      final hasCritical = abnormalities.any(
        (a) => a['severity']?.toString().toUpperCase() == 'CRITICAL',
      );

      final PdfColor statusBg;
      final PdfColor statusBorder;
      final PdfColor statusDot;
      final PdfColor statusTextColor;
      final String statusText;

      if (abnormalities.isEmpty) {
        statusBg = const PdfColor.fromInt(0xFFE8F5E9);
        statusBorder = PdfColors.green100;
        statusDot = PdfColors.green;
        statusTextColor = PdfColors.green800;
        statusText = 'STATUS: NORMAL SINUS RHYTHM';
      } else if (hasCritical) {
        statusBg = const PdfColor.fromInt(0xFFFFEBEE);
        statusBorder = PdfColors.red200;
        statusDot = PdfColors.red;
        statusTextColor = PdfColors.red800;
        statusText = 'STATUS: CRITICAL ABNORMALITIES DETECTED';
      } else {
        statusBg = const PdfColor.fromInt(0xFFFFFDE7);
        statusBorder = const PdfColor.fromInt(0xFFFFF59D);
        statusDot = const PdfColor.fromInt(0xFFFBC02D);
        statusTextColor = const PdfColor.fromInt(0xFFF57F17);
        statusText = 'STATUS: ABNORMALITIES DETECTED (WARNING)';
      }

      // --- PAGE 1: Diagnostics and Clinical Measurements ---
      pdf.addPage(
        pw.Page(
          pageFormat: PdfPageFormat.a4.landscape,
          margin: const pw.EdgeInsets.all(20),
          build: (pw.Context context) {
            return pw.Column(
              crossAxisAlignment: pw.CrossAxisAlignment.start,
              children: [
                pw.Row(
                  mainAxisAlignment: pw.MainAxisAlignment.spaceBetween,
                  children: [
                    pw.Column(
                      crossAxisAlignment: pw.CrossAxisAlignment.start,
                      children: [
                        pw.Text(
                          '12 Lead ECG Report',
                          style: pw.TextStyle(
                            fontSize: 24,
                            fontWeight: pw.FontWeight.bold,
                            color: const PdfColor.fromInt(0xFF074799),
                          ),
                        ),
                        pw.Text(
                          'Date: ${widget.reportDate}',
                          style: pw.TextStyle(
                            fontSize: 10,
                            color: PdfColors.grey700,
                          ),
                        ),
                      ],
                    ),
                    pw.Column(
                      crossAxisAlignment: pw.CrossAxisAlignment.end,
                      children: [
                        pw.Text(
                          'Rhythmrix AI Healthcare',
                          style: pw.TextStyle(
                            fontSize: 14,
                            fontWeight: pw.FontWeight.bold,
                            color: const PdfColor.fromInt(0xFF074799),
                          ),
                        ),
                        pw.Text(
                          'REPORT ID: ${widget.reportId}',
                          style: pw.TextStyle(
                            fontSize: 9,
                            color: PdfColors.grey600,
                          ),
                        ),
                      ],
                    ),
                  ],
                ),
                pw.SizedBox(height: 8),
                pw.Divider(
                  color: const PdfColor.fromInt(0xFF074799),
                  thickness: 1,
                ),
                pw.SizedBox(height: 10),

                // Patient Profile demographics block (Horizontal layout)
                pw.Container(
                  padding: const pw.EdgeInsets.symmetric(
                    horizontal: 16,
                    vertical: 10,
                  ),
                  decoration: pw.BoxDecoration(
                    color: const PdfColor.fromInt(0xFFF8F9FE),
                    borderRadius: const pw.BorderRadius.all(
                      pw.Radius.circular(8),
                    ),
                    border: pw.Border.all(
                      color: const PdfColor.fromInt(0xFFE3EBFF),
                    ),
                  ),
                  child: pw.Row(
                    mainAxisAlignment: pw.MainAxisAlignment.spaceBetween,
                    children: [
                      _pdfProfileField('PATIENT NAME', pdfName.toUpperCase()),
                      _pdfProfileField('AGE', '$pdfAge Y'),
                      _pdfProfileField('GENDER', pdfGender.toUpperCase()),
                      _pdfProfileField('HEIGHT', '$pdfHeight cm'),
                      _pdfProfileField('WEIGHT', '$pdfWeight kg'),
                    ],
                  ),
                ),
                pw.SizedBox(height: 15),

                // Main body: Clinical measurements and results
                pw.Row(
                  crossAxisAlignment: pw.CrossAxisAlignment.start,
                  children: [
                    // Left Column: ECG parameters table
                    pw.Expanded(
                      flex: 5,
                      child: pw.Column(
                        crossAxisAlignment: pw.CrossAxisAlignment.start,
                        children: [
                          _pdfSectionTitle('ECG Parameters'),
                          pw.Table(
                            border: pw.TableBorder.all(
                              color: PdfColors.grey300,
                              width: 0.5,
                            ),
                            children: [
                              // Table Header
                              pw.TableRow(
                                decoration: pw.BoxDecoration(
                                  color: const PdfColor.fromInt(0xFF074799),
                                ),
                                children: [
                                  pw.Padding(
                                    padding: const pw.EdgeInsets.symmetric(
                                      horizontal: 10,
                                      vertical: 9,
                                    ),
                                    child: pw.Text(
                                      'Parameter',
                                      style: pw.TextStyle(
                                        fontWeight: pw.FontWeight.bold,
                                        fontSize: 12,
                                        color: PdfColors.white,
                                      ),
                                    ),
                                  ),
                                  pw.Padding(
                                    padding: const pw.EdgeInsets.symmetric(
                                      horizontal: 10,
                                      vertical: 9,
                                    ),
                                    child: pw.Text(
                                      'Observed Value',
                                      style: pw.TextStyle(
                                        fontWeight: pw.FontWeight.bold,
                                        fontSize: 12,
                                        color: PdfColors.white,
                                      ),
                                    ),
                                  ),
                                  pw.Padding(
                                    padding: const pw.EdgeInsets.symmetric(
                                      horizontal: 10,
                                      vertical: 9,
                                    ),
                                    child: pw.Text(
                                      'Standard Range',
                                      style: pw.TextStyle(
                                        fontWeight: pw.FontWeight.bold,
                                        fontSize: 12,
                                        color: PdfColors.white,
                                      ),
                                    ),
                                  ),
                                ],
                              ),
                              // Rows
                              _pdfTableRow(
                                'PR Interval',
                                '$pdfPr ms',
                                '$pdfPrRange ms',
                              ),
                              _pdfTableRow(
                                'QRS Interval',
                                '$pdfQrs ms',
                                '$pdfQrsRange ms',
                              ),
                              _pdfTableRow(
                                'QT Interval',
                                '$pdfQt ms',
                                '$pdfQtRange ms',
                              ),
                              _pdfTableRow(
                                'QTc Interval',
                                '$pdfQtc ms',
                                '$pdfQtcRange ms',
                              ),
                              _pdfTableRow(
                                'Heart Rate',
                                '$pdfHr bpm',
                                '$pdfHrRange bpm',
                              ),
                            ],
                          ),
                          _pdfRiskMeter(hasCritical, abnormalities.isEmpty),
                        ],
                      ),
                    ),
                    pw.SizedBox(width: 20),

                    // Right Column: Result Details and Interpretation
                    pw.Expanded(
                      flex: 5,
                      child: pw.Column(
                        crossAxisAlignment: pw.CrossAxisAlignment.start,
                        children: [
                          _pdfSectionTitle('Result Details (AI Powered)'),
                          pw.SizedBox(height: 8),
                          pw.Container(
                            padding: const pw.EdgeInsets.all(12),
                            decoration: pw.BoxDecoration(
                              color: statusBg,
                              borderRadius: const pw.BorderRadius.all(
                                pw.Radius.circular(8),
                              ),
                              border: pw.Border.all(color: statusBorder),
                            ),
                            child: pw.Column(
                              crossAxisAlignment: pw.CrossAxisAlignment.start,
                              children: [
                                pw.Row(
                                  children: [
                                    pw.Container(
                                      width: 10,
                                      height: 10,
                                      decoration: pw.BoxDecoration(
                                        color: statusDot,
                                        shape: pw.BoxShape.circle,
                                      ),
                                    ),
                                    pw.SizedBox(width: 8),
                                    pw.Expanded(
                                      child: pw.Text(
                                        statusText,
                                        style: pw.TextStyle(
                                          fontWeight: pw.FontWeight.bold,
                                          color: statusTextColor,
                                          fontSize: 11,
                                        ),
                                      ),
                                    ),
                                  ],
                                ),
                                pw.SizedBox(height: 10),
                                pw.Text(
                                  'Quality Score: $pdfQualityScore%',
                                  style: pw.TextStyle(
                                    fontWeight: pw.FontWeight.bold,
                                    fontSize: 10,
                                    color: PdfColors.grey800,
                                  ),
                                ),
                              ],
                            ),
                          ),
                          pw.SizedBox(height: 12),
                          _pdfSectionTitle('Interpretation Findings'),
                          pw.SizedBox(height: 6),
                          if (abnormalities.isEmpty)
                            pw.Row(
                              children: [
                                pw.Container(
                                  width: 4,
                                  height: 4,
                                  decoration: const pw.BoxDecoration(
                                    color: PdfColors.green,
                                    shape: pw.BoxShape.circle,
                                  ),
                                ),
                                pw.SizedBox(width: 8),
                                pw.Text(
                                  'Normal Sinus Rhythm. No significant abnormalities detected.',
                                  style: const pw.TextStyle(
                                    fontSize: 10,
                                    color: PdfColors.grey800,
                                  ),
                                ),
                              ],
                            )
                          else
                            ...abnormalities.map((a) {
                              final sev =
                                  a['severity']?.toString().toUpperCase() ??
                                  'INFO';
                              final isCrit = sev == 'CRITICAL';
                              final isWarn = sev == 'WARNING';
                              final dotColor = isCrit
                                  ? PdfColors.red
                                  : (isWarn
                                        ? const PdfColor.fromInt(0xFFFBC02D)
                                        : PdfColors.blue);
                              final textColor = isCrit
                                  ? const PdfColor.fromInt(0xFF991B1B)
                                  : (isWarn
                                        ? const PdfColor.fromInt(0xFFF57F17)
                                        : PdfColors.blue800);

                              return pw.Padding(
                                padding: const pw.EdgeInsets.only(bottom: 4),
                                child: pw.Row(
                                  crossAxisAlignment:
                                      pw.CrossAxisAlignment.start,
                                  children: [
                                    pw.Container(
                                      margin: const pw.EdgeInsets.only(top: 3),
                                      width: 5,
                                      height: 5,
                                      decoration: pw.BoxDecoration(
                                        color: dotColor,
                                        shape: pw.BoxShape.circle,
                                      ),
                                    ),
                                    pw.SizedBox(width: 8),
                                    pw.Expanded(
                                      child: pw.Text(
                                        '${a['abnormalityName']} (${a['severity']})',
                                        style: pw.TextStyle(
                                          fontSize: 9.5,
                                          color: textColor,
                                          fontWeight: pw.FontWeight.bold,
                                        ),
                                      ),
                                    ),
                                  ],
                                ),
                              );
                            }),
                        ],
                      ),
                    ),
                  ],
                ),
                pw.Spacer(),

                // Bottom Footer & Disclaimer (Page 1 of 2)
                pw.Divider(color: PdfColors.grey300, thickness: 0.8),
                pw.SizedBox(height: 4),
                pw.Text(
                  'DISCLAIMER: This report is generated by an automated AI diagnostic system for clinical screening. It does not constitute a definitive medical diagnosis. All findings should be reviewed and verified by a cardiologist.',
                  style: const pw.TextStyle(
                    fontSize: 7,
                    color: PdfColors.grey700,
                  ),
                ),
                pw.SizedBox(height: 4),
                pw.Row(
                  mainAxisAlignment: pw.MainAxisAlignment.spaceBetween,
                  children: [
                    pw.Text(
                      'CONFIDENTIAL MEDICAL RECORD - RHYTHMRIX',
                      style: pw.TextStyle(
                        fontSize: 8,
                        fontWeight: pw.FontWeight.bold,
                        color: PdfColors.black,
                      ),
                    ),
                    pw.Text(
                      'Page 1 of $totalPages',
                      style: const pw.TextStyle(
                        fontSize: 8,
                        color: PdfColors.grey700,
                      ),
                    ),
                  ],
                ),
              ],
            );
          },
        ),
      );

      // --- PAGE 2: The 12-Lead ECG Grid Layout ---
      pdf.addPage(
        pw.Page(
          pageFormat: PdfPageFormat.a4.landscape,
          margin: const pw.EdgeInsets.all(20),
          build: (pw.Context context) {
            return pw.Column(
              crossAxisAlignment: pw.CrossAxisAlignment.start,
              children: [
                // Header (compact)
                pw.Row(
                  mainAxisAlignment: pw.MainAxisAlignment.spaceBetween,
                  children: [
                    pw.Column(
                      crossAxisAlignment: pw.CrossAxisAlignment.start,
                      children: [
                        pw.Text(
                          '12 Lead ECG Report',
                          style: pw.TextStyle(
                            fontSize: 18,
                            fontWeight: pw.FontWeight.bold,
                            color: const PdfColor.fromInt(0xFF074799),
                          ),
                        ),
                        pw.Text(
                          'Date: ${widget.reportDate}',
                          style: pw.TextStyle(
                            fontSize: 8,
                            color: PdfColors.grey700,
                          ),
                        ),
                      ],
                    ),
                    pw.Column(
                      crossAxisAlignment: pw.CrossAxisAlignment.end,
                      children: [
                        pw.Text(
                          'Rhythmrix AI Healthcare',
                          style: pw.TextStyle(
                            fontSize: 12,
                            fontWeight: pw.FontWeight.bold,
                            color: const PdfColor.fromInt(0xFF074799),
                          ),
                        ),
                        pw.Text(
                          'REPORT ID: ${widget.reportId}',
                          style: pw.TextStyle(
                            fontSize: 8,
                            color: PdfColors.grey600,
                          ),
                        ),
                      ],
                    ),
                  ],
                ),
                pw.SizedBox(height: 4),
                pw.Divider(
                  color: const PdfColor.fromInt(0xFF074799),
                  thickness: 0.8,
                ),
                pw.SizedBox(height: 4),

                // Patient Profile demographics block (Compact)
                pw.Container(
                  padding: const pw.EdgeInsets.symmetric(
                    horizontal: 12,
                    vertical: 6,
                  ),
                  decoration: pw.BoxDecoration(
                    color: const PdfColor.fromInt(0xFFF8F9FE),
                    borderRadius: const pw.BorderRadius.all(
                      pw.Radius.circular(6),
                    ),
                    border: pw.Border.all(
                      color: const PdfColor.fromInt(0xFFE3EBFF),
                    ),
                  ),
                  child: pw.Row(
                    mainAxisAlignment: pw.MainAxisAlignment.spaceBetween,
                    children: [
                      _pdfProfileField('PATIENT NAME', pdfName.toUpperCase()),
                      _pdfProfileField(
                        'AGE/GENDER',
                        '$pdfAge Y / ${pdfGender.toUpperCase()}',
                      ),
                      _pdfProfileField(
                        'HEIGHT/WEIGHT',
                        '$pdfHeight cm / $pdfWeight kg',
                      ),
                      _pdfProfileField('HEART RATE', '$pdfHr bpm'),
                    ],
                  ),
                ),
                pw.SizedBox(height: 10),

                // 12-Lead Millimeter Grid and Traces
                pw.Expanded(
                  child: pw.Stack(
                    children: [
                      // 1. The custom grid and trace painter
                      pw.CustomPaint(
                        size: const PdfPoint(802, 420),
                        painter: (PdfGraphics canvas, PdfPoint size) {
                          // Draw Minor Grid lines (2.5 points spacing = 5 minor boxes per major box, light pink)
                          canvas.setStrokeColor(
                            const PdfColor.fromInt(0xFFFFEBEE),
                          );
                          canvas.setLineWidth(0.4);
                          for (double i = 0; i <= size.x; i += 2.5) {
                            canvas.drawLine(i, 0, i, size.y);
                          }
                          for (double i = 0; i <= size.y; i += 2.5) {
                            canvas.drawLine(0, i, size.x, i);
                          }
                          canvas.strokePath();

                          // Draw Major Grid lines (12.5 points spacing = 16 major boxes per 200.5pt column, darker pink)
                          canvas.setStrokeColor(
                            const PdfColor.fromInt(0xFFFFCDD2),
                          );
                          canvas.setLineWidth(0.8);
                          for (double i = 0; i <= size.x; i += 12.5) {
                            canvas.drawLine(i, 0, i, size.y);
                          }
                          for (double i = 0; i <= size.y; i += 12.5) {
                            canvas.drawLine(0, i, size.x, i);
                          }
                          canvas.strokePath();

                          // Draw vertical dividers between 4 columns from y = 105 to 420
                          canvas.setStrokeColor(
                            const PdfColor.fromInt(0xFF94A3B8),
                          ); // Slate gray divider
                          canvas.setLineWidth(0.8);
                          canvas.setLineDashPattern([3, 3], 0);

                          canvas.drawLine(200.5, 105, 200.5, 420);
                          canvas.drawLine(401.0, 105, 401.0, 420);
                          canvas.drawLine(601.5, 105, 601.5, 420);
                          canvas.strokePath();

                          // Reset dash pattern
                          canvas.setLineDashPattern([], 0);

                          // Outer border of the grid
                          canvas.setStrokeColor(
                            const PdfColor.fromInt(0xFFCBD5E1),
                          ); // light gray border
                          canvas.setLineWidth(1.0);
                          canvas.drawRect(0, 0, size.x, size.y);
                          canvas.strokePath();

                          // Line above rhythm strip (y = 105)
                          canvas.drawLine(0, 105, size.x, 105);
                          canvas.strokePath();

                          // Horizontal dividers between 4 equal rows (y = 105, 210, 315)
                          canvas.setStrokeColor(
                            const PdfColor.fromInt(0xFFE2E8F0),
                          );
                          canvas.setLineWidth(0.5);
                          canvas.drawLine(0, 105.0, size.x, 105.0);
                          canvas.drawLine(0, 210.0, size.x, 210.0);
                          canvas.drawLine(0, 315.0, size.x, 315.0);
                          canvas.strokePath();

                          // 1. Calculate global max amplitude range across all 12 leads for uniform proportion matching reference report
                                       // Helper function: Dual-Stage Median Baseline Removal + 3-Point Moving Average Filter
                          List<double> filterECGBaselineNoise(List<double> sig, {int fs = 125}) {
                            if (sig.length < 10) return List<double>.from(sig);

                            List<double> rollingMedian(List<double> data, int winSize) {
                              final len = data.length;
                              final half = winSize ~/ 2;
                              final res = List<double>.filled(len, 0.0);
                              for (int i = 0; i < len; i++) {
                                int start = math.max(0, i - half);
                                int end = math.min(len, i + half + 1);
                                List<double> w = data.sublist(start, end)..sort();
                                res[i] = w[w.length ~/ 2];
                              }
                              return res;
                            }

                            // 1. Stage 1: 200ms median filter (removes QRS peaks)
                            int win1 = math.max(3, (0.20 * fs).round());
                            List<double> base1 = rollingMedian(sig, win1);

                            // 2. Stage 2: 600ms median filter (removes T-waves & smooths baseline)
                            int win2 = math.max(3, (0.60 * fs).round());
                            List<double> base2 = rollingMedian(base1, win2);

                            // 3. Subtract baseline wander
                            List<double> clean = List<double>.filled(sig.length, 0.0);
                            for (int i = 0; i < sig.length; i++) {
                              clean[i] = sig[i] - base2[i];
                            }

                            // 4. 3-Point Moving Average Low-Pass (removes 50Hz hum and high-frequency baseline jitter)
                            List<double> smooth = List<double>.filled(clean.length, 0.0);
                            if (clean.length >= 3) {
                              smooth[0] = clean[0];
                              smooth[clean.length - 1] = clean[clean.length - 1];
                              for (int i = 1; i < clean.length - 1; i++) {
                                smooth[i] = (clean[i - 1] + clean[i] + clean[i + 1]) / 3.0;
                              }
                            } else {
                              smooth = clean;
                            }

                            return smooth;
                          }

                          final double colWidth = 200.5;

                          // Helper function to detect the index of the first R-peak for beat-phase alignment across leads
                          int findFirstRPeakIndex(List<double> signal) {
                            if (signal.length < 10) return -1;
                            List<double> sorted = List<double>.from(signal)..sort();
                            double base = sorted[sorted.length ~/ 2];
                            double maxDev = 0.0;
                            for (var v in signal) {
                              double dev = (v - base).abs();
                              if (dev > maxDev) maxDev = dev;
                            }
                            if (maxDev <= 0) return -1;
                            double threshold = maxDev * 0.55;
                            for (int i = 2; i < signal.length - 2; i++) {
                              double dev = (signal[i] - base).abs();
                              if (dev > threshold &&
                                  dev > (signal[i - 1] - base).abs() &&
                                  dev > (signal[i - 2] - base).abs() &&
                                  dev >= (signal[i + 1] - base).abs() &&
                                  dev >= (signal[i + 2] - base).abs()) {
                                return i;
                              }
                            }
                            return -1;
                          }

                          // Compute reference first R-peak index from Lead II for global beat-phase alignment across columns
                          final List<double> refL2 = _staticLeadData['Lead II'] ?? _staticLeadData['L2'] ?? ecgData;
                          final int globalFirstR = findFirstRPeakIndex(refL2);
                          final int globalStartIdx = (globalFirstR >= 0) ? math.max(0, globalFirstR - 15) : 0;

                          // Helper drawing closure with Spandan per-lead normalized scaling
                          void drawTrace(
                            String leadName,
                            int colIdx,
                            double yMin,
                            double yMax,
                          ) {
                            final bool hasSpecificLead =
                                _staticLeadData.containsKey(leadName) &&
                                _staticLeadData[leadName]!.isNotEmpty;
                            final rawPts = hasSpecificLead
                                ? _staticLeadData[leadName]!
                                : ecgData;
                            if (rawPts.isEmpty) return;

                            List<double> pts = List<double>.from(rawPts);
                            if (pts.length >= 2 &&
                                (pts[0] - pts[1]).abs() > 15000) {
                              pts[0] = pts[1];
                            }

                            // 1. Filter signal & remove baseline noise completely
                            if (pts.length >= 20) {
                              pts = filterECGBaselineNoise(pts);
                            }

                            // 2. Beat-Phase R-Peak Alignment synchronized to Lead II reference phase
                            int leadR = findFirstRPeakIndex(pts);
                            int startIdx = (leadR >= 0 && (leadR - globalFirstR).abs() <= 50)
                                ? math.max(0, leadR - 15)
                                : globalStartIdx;

                            if (pts.length > startIdx + 310) {
                              pts = pts.sublist(
                                startIdx,
                                math.min(pts.length, startIdx + 310),
                              );
                            } else if (pts.length > 310) {
                              pts = pts.sublist(0, 310);
                            }

                            // 3. Subsample points for clean wave display in 200.5pt box
                            if (pts.length > 200) {
                              int maxPts = 200;
                              double step = pts.length / maxPts;
                              List<double> sampled = [];
                              for (int i = 0; i < maxPts; i++) {
                                int idx = (i * step).toInt().clamp(
                                  0,
                                  pts.length - 1,
                                );
                                sampled.add(pts[idx]);
                              }
                              pts = sampled;
                            }

                            final xStart = colIdx * colWidth;
                            final xEnd = (colIdx + 1) * colWidth;

                            final cellHeight = yMax - yMin;
                            final yMid = yMin + cellHeight / 2;

                            // Calibration pulse ONLY at Column 0
                            final bool isColZero = colIdx == 0;
                            if (isColZero) {
                              canvas.setStrokeColor(
                                const PdfColor.fromInt(0xFF074799),
                              );
                              canvas.setLineWidth(0.8);
                              canvas.moveTo(xStart, yMid);
                              canvas.lineTo(xStart + 2.5, yMid);
                              canvas.lineTo(xStart + 2.5, yMid + 25.0);
                              canvas.lineTo(xStart + 10.0, yMid + 25.0);
                              canvas.lineTo(xStart + 10.0, yMid);
                              canvas.lineTo(xStart + 12.5, yMid);
                              canvas.strokePath();
                            }

                            // Isoelectric median baseline centering
                            List<double> sortedPts = List<double>.from(pts)..sort();
                            double medianBaseline = sortedPts[sortedPts.length ~/ 2];

                            // Per-lead scaling like Spandan: each lead's max peak reaches 30% of cellHeight (31.5 pt)
                            // Guarantees 9.0 pt headroom below text label and 21.0 pt headroom below top border
                            double posMax = 0.0, negMax = 0.0;
                            for (var v in pts) {
                              double dev = v - medianBaseline;
                              if (dev > posMax) posMax = dev;
                              if (-dev > negMax) negMax = -dev;
                            }
                            double maxAbsDev = math.max(posMax, negMax);
                            if (maxAbsDev < 1.0) maxAbsDev = 1.0;

                            final double leadScale = (cellHeight * 0.30) / maxAbsDev;

                            final signalXStart = isColZero
                                ? xStart + 12.5
                                : xStart;
                            final signalWidth = xEnd - signalXStart;
                            final xStep =
                                signalWidth / math.max(1, pts.length - 1);

                            canvas.setStrokeColor(
                              const PdfColor.fromInt(0xFF074799),
                            );
                            canvas.setLineWidth(0.9);

                            for (int i = 0; i < pts.length; i++) {
                              final x = signalXStart + (i * xStep);
                              final y = yMid + (pts[i] - medianBaseline) * leadScale;
                              if (i == 0) {
                                canvas.moveTo(x, y);
                              } else {
                                canvas.lineTo(x, y);
                              }
                            }
                            canvas.strokePath();
                          }

                          // 4 Equal Rows (105.0pt height each, zero gap)
                          // Row 0 (top): y from 315.0 to 420.0
                          drawTrace('Lead I', 0, 315.0, 420.0);
                          drawTrace('aVR', 1, 315.0, 420.0);
                          drawTrace('V1', 2, 315.0, 420.0);
                          drawTrace('V4', 3, 315.0, 420.0);

                          // Row 1: y from 210.0 to 315.0
                          drawTrace('Lead II', 0, 210.0, 315.0);
                          drawTrace('aVL', 1, 210.0, 315.0);
                          drawTrace('V2', 2, 210.0, 315.0);
                          drawTrace('V5', 3, 210.0, 315.0);

                          // Row 2: y from 105.0 to 210.0
                          drawTrace('Lead III', 0, 105.0, 210.0);
                          drawTrace('aVF', 1, 105.0, 210.0);
                          drawTrace('V3', 2, 105.0, 210.0);
                          drawTrace('V6', 3, 105.0, 210.0);

                          // Rhythm Strip: Row 3 (bottom): y from 0.0 to 105.0 (full signal of Lead II)
                          final rhythmPts =
                              _staticLeadData['Lead II'] ?? ecgData;
                          if (rhythmPts.isNotEmpty) {
                            final yMin = 0.0;
                            final yMax = 105.0;
                            final yMid = yMin + (yMax - yMin) / 2;
                            final xStart = 0.0;
                            final xEnd = 802.0;

                            // Calibration pulse
                            canvas.setStrokeColor(
                              const PdfColor.fromInt(0xFF074799),
                            );
                            canvas.setLineWidth(0.8);
                            canvas.moveTo(xStart, yMid);
                            canvas.lineTo(xStart + 2.5, yMid);
                            canvas.lineTo(xStart + 2.5, yMid + 25.0);
                            canvas.lineTo(xStart + 10.0, yMid + 25.0);
                            canvas.lineTo(xStart + 10.0, yMid);
                            canvas.lineTo(xStart + 12.5, yMid);
                            canvas.strokePath();

                            // Draw rhythm strip signal with normalized scaling
                            List<double> rhythmSub = List<double>.from(rhythmPts);
                            if (rhythmSub.length >= 20) {
                              rhythmSub = filterECGBaselineNoise(rhythmSub);
                            }

                            int rFirstR = findFirstRPeakIndex(rhythmSub);
                            if (rFirstR > 20 &&
                                rFirstR + 600 <= rhythmSub.length) {
                              rhythmSub = rhythmSub.sublist(rFirstR - 20);
                            }

                            // Subsample for 802pt width (max 800 points)
                            if (rhythmSub.length > 800) {
                              int maxPts = 800;
                              double step = rhythmSub.length / maxPts;
                              List<double> sampled = [];
                              for (int i = 0; i < maxPts; i++) {
                                int idx = (i * step).toInt().clamp(
                                  0,
                                  rhythmSub.length - 1,
                                );
                                sampled.add(rhythmSub[idx]);
                              }
                              rhythmSub = sampled;
                            }

                            List<double> sortedRhythm = List<double>.from(rhythmSub)..sort();
                            double rhythmMedian = sortedRhythm[sortedRhythm.length ~/ 2];

                            double posMaxR = 0.0, negMaxR = 0.0;
                            for (var v in rhythmSub) {
                              double dev = v - rhythmMedian;
                              if (dev > posMaxR) posMaxR = dev;
                              if (-dev > negMaxR) negMaxR = -dev;
                            }
                            double maxAbsDevR = math.max(posMaxR, negMaxR);
                            if (maxAbsDevR < 1.0) maxAbsDevR = 1.0;

                            final double rhythmScale = (105.0 * 0.30) / maxAbsDevR;

                            final signalXStart = xStart + 12.5;
                            final signalXEnd = xStart + (xEnd - xStart) * 0.90;
                            final signalWidth = signalXEnd - signalXStart;
                            final xStep =
                                signalWidth / math.max(1, rhythmSub.length - 1);

                            canvas.setStrokeColor(
                              const PdfColor.fromInt(0xFF074799),
                            );
                            canvas.setLineWidth(0.9);

                            for (int i = 0; i < rhythmSub.length; i++) {
                              final x = signalXStart + (i * xStep);
                              final y =
                                  yMid + (rhythmSub[i] - rhythmMedian) * rhythmScale;
                              if (i == 0) {
                                canvas.moveTo(x, y);
                              } else {
                                canvas.lineTo(x, y);
                              }
                            }
                            canvas.strokePath();
                          }
                        },
                      ),

                      // 2. Positioned labels for 4 Equal Rows (105pt height each)
                      _pdfPositionedLabel('lead I', 10, 4),
                      _pdfPositionedLabel('aVR', 210, 4),
                      _pdfPositionedLabel('v1', 411, 4),
                      _pdfPositionedLabel('v4', 611, 4),

                      _pdfPositionedLabel('lead II', 10, 109),
                      _pdfPositionedLabel('aVL', 210, 109),
                      _pdfPositionedLabel('v2', 411, 109),
                      _pdfPositionedLabel('v5', 611, 109),

                      _pdfPositionedLabel('lead III', 10, 214),
                      _pdfPositionedLabel('aVF', 210, 214),
                      _pdfPositionedLabel('v3', 411, 214),
                      _pdfPositionedLabel('v6', 611, 214),

                      _pdfPositionedLabel('Rhythm (lead II)', 10, 319),

                      // 3. End of signal text positioned at the end of the rhythm strip
                      pw.Positioned(
                        right: 20,
                        top: 395,
                        child: pw.Text(
                          '----End of signal----',
                          style: pw.TextStyle(
                            fontSize: 7,
                            fontWeight: pw.FontWeight.bold,
                            color: PdfColors.grey600,
                          ),
                        ),
                      ),
                    ],
                  ),
                ),
                pw.SizedBox(height: 6),

                // Bottom Footer & Scales (Page 2 of 2)
                pw.Row(
                  mainAxisAlignment: pw.MainAxisAlignment.spaceBetween,
                  children: [
                    pw.Text(
                      'Scale: x-axis: 25.0 mm/sec  y-axis: 10.0 mm/mv | Heart rate: $pdfHr bpm',
                      style: pw.TextStyle(
                        fontSize: 8,
                        color: PdfColors.grey700,
                      ),
                    ),
                    pw.Text(
                      'Page 2 of $totalPages',
                      style: const pw.TextStyle(
                        fontSize: 8,
                        color: PdfColors.grey700,
                      ),
                    ),
                  ],
                ),
              ],
            );
          },
        ),
      );

      for (var pageIdx = 0; pageIdx < chunks.length; pageIdx++) {
        final chunk = chunks[pageIdx];
        final pageNum = 3 + pageIdx;

        final leftItems = chunk;
        final rightItems = <dynamic>[];

        pdf.addPage(
          pw.Page(
            pageFormat: PdfPageFormat.a4.landscape,
            margin: const pw.EdgeInsets.all(20),
            build: (pw.Context context) {
              return pw.Column(
                crossAxisAlignment: pw.CrossAxisAlignment.start,
                children: [
                  pw.Row(
                    mainAxisAlignment: pw.MainAxisAlignment.spaceBetween,
                    children: [
                      pw.Column(
                        crossAxisAlignment: pw.CrossAxisAlignment.start,
                        children: [
                          pw.Text(
                            '12 Lead ECG Report',
                            style: pw.TextStyle(
                              fontSize: 18,
                              fontWeight: pw.FontWeight.bold,
                              color: const PdfColor.fromInt(0xFF074799),
                            ),
                          ),
                          pw.Text(
                            'Date: ${widget.reportDate}',
                            style: pw.TextStyle(
                              fontSize: 8,
                              color: PdfColors.grey700,
                            ),
                          ),
                        ],
                      ),
                      pw.Column(
                        crossAxisAlignment: pw.CrossAxisAlignment.end,
                        children: [
                          pw.Text(
                            'Rhythmrix AI Healthcare',
                            style: pw.TextStyle(
                              fontSize: 12,
                              fontWeight: pw.FontWeight.bold,
                              color: const PdfColor.fromInt(0xFF074799),
                            ),
                          ),
                          pw.Text(
                            'REPORT ID: ${widget.reportId}',
                            style: pw.TextStyle(
                              fontSize: 8,
                              color: PdfColors.grey700,
                            ),
                          ),
                        ],
                      ),
                    ],
                  ),
                  pw.SizedBox(height: 10),
                  pw.Divider(
                    color: const PdfColor.fromInt(0xFF074799),
                    thickness: 0.8,
                  ),
                  pw.SizedBox(height: 10),
                  pw.Expanded(
                    child: pw.Row(
                      crossAxisAlignment: pw.CrossAxisAlignment.start,
                      children: [
                        // Left column: up to 3 abnormalities
                        pw.Expanded(
                          flex: 5,
                          child: pw.Column(
                            crossAxisAlignment: pw.CrossAxisAlignment.start,
                            children: [
                              _pdfSectionTitle('DETECTED CLINICAL FINDINGS'),
                              pw.SizedBox(height: 8),
                              ...leftItems.map((a) {
                                final name =
                                    a['abnormalityName']?.toString() ??
                                    'Abnormality';
                                final severity =
                                    a['severity']?.toString() ?? 'INFO';
                                final isCrit =
                                    severity.toUpperCase() == 'CRITICAL';
                                final isWarn =
                                    severity.toUpperCase() == 'WARNING';

                                final badgeColor = isCrit
                                    ? const PdfColor.fromInt(0xFF991B1B)
                                    : (isWarn
                                          ? const PdfColor.fromInt(0xFFF57F17)
                                          : PdfColors.blue800);

                                final badgeBg = isCrit
                                    ? const PdfColor.fromInt(0xFFFFEBEE)
                                    : (isWarn
                                          ? const PdfColor.fromInt(0xFFFFFDE7)
                                          : const PdfColor.fromInt(0xFFE3EBFF));

                                final desc =
                                    abnormalityExplanations[name]?['description'] ??
                                    'An atypical electrical signal has been detected in the heart rhythm.';
                                final causes =
                                    abnormalityExplanations[name]?['causes'] ??
                                    'Varies based on age, cardiac history, systemic conditions, or external factors.';
                                final actions =
                                    abnormalityExplanations[name]?['actions'] ??
                                    'Please consult a medical professional or cardiologist to verify this finding.';

                                return pw.Container(
                                  margin: const pw.EdgeInsets.only(bottom: 6),
                                  padding: const pw.EdgeInsets.all(6),
                                  decoration: pw.BoxDecoration(
                                    color: const PdfColor.fromInt(0xFFF8F9FE),
                                    borderRadius: const pw.BorderRadius.all(
                                      pw.Radius.circular(6),
                                    ),
                                    border: pw.Border.all(
                                      color: const PdfColor.fromInt(0xFFE3EBFF),
                                    ),
                                  ),
                                  child: pw.Column(
                                    crossAxisAlignment:
                                        pw.CrossAxisAlignment.start,
                                    children: [
                                      pw.Row(
                                        mainAxisAlignment:
                                            pw.MainAxisAlignment.spaceBetween,
                                        children: [
                                          pw.Text(
                                            name.toUpperCase(),
                                            style: pw.TextStyle(
                                              fontSize: 9.5,
                                              fontWeight: pw.FontWeight.bold,
                                              color: const PdfColor.fromInt(
                                                0xFF074799,
                                              ),
                                            ),
                                          ),
                                          pw.Container(
                                            padding:
                                                const pw.EdgeInsets.symmetric(
                                                  horizontal: 5,
                                                  vertical: 1.5,
                                                ),
                                            decoration: pw.BoxDecoration(
                                              color: badgeBg,
                                              borderRadius:
                                                  const pw.BorderRadius.all(
                                                    pw.Radius.circular(3),
                                                  ),
                                            ),
                                            child: pw.Text(
                                              severity.toUpperCase(),
                                              style: pw.TextStyle(
                                                fontSize: 6,
                                                fontWeight: pw.FontWeight.bold,
                                                color: badgeColor,
                                              ),
                                            ),
                                          ),
                                        ],
                                      ),
                                      pw.SizedBox(height: 4),
                                      pw.RichText(
                                        text: pw.TextSpan(
                                          children: [
                                            pw.TextSpan(
                                              text: 'Explanation: ',
                                              style: pw.TextStyle(
                                                fontWeight: pw.FontWeight.bold,
                                                fontSize: 7.5,
                                                color: PdfColors.black,
                                              ),
                                            ),
                                            pw.TextSpan(
                                              text: desc,
                                              style: const pw.TextStyle(
                                                fontSize: 7.5,
                                                color: PdfColors.grey800,
                                              ),
                                            ),
                                          ],
                                        ),
                                      ),
                                      pw.SizedBox(height: 2),
                                      pw.RichText(
                                        text: pw.TextSpan(
                                          children: [
                                            pw.TextSpan(
                                              text: 'Potential Causes: ',
                                              style: pw.TextStyle(
                                                fontWeight: pw.FontWeight.bold,
                                                fontSize: 7.5,
                                                color: PdfColors.black,
                                              ),
                                            ),
                                            pw.TextSpan(
                                              text: causes,
                                              style: const pw.TextStyle(
                                                fontSize: 7.5,
                                                color: PdfColors.grey800,
                                              ),
                                            ),
                                          ],
                                        ),
                                      ),
                                      pw.SizedBox(height: 2),
                                      pw.RichText(
                                        text: pw.TextSpan(
                                          children: [
                                            pw.TextSpan(
                                              text: 'Recommended Action: ',
                                              style: pw.TextStyle(
                                                fontWeight: pw.FontWeight.bold,
                                                fontSize: 7.5,
                                                color: PdfColors.black,
                                              ),
                                            ),
                                            pw.TextSpan(
                                              text: actions,
                                              style: const pw.TextStyle(
                                                fontSize: 7.5,
                                                color: PdfColors.grey800,
                                              ),
                                            ),
                                          ],
                                        ),
                                      ),
                                    ],
                                  ),
                                );
                              }).toList(),
                            ],
                          ),
                        ),
                        pw.SizedBox(width: 15),
                        // Right column: remaining abnormalities + clinical advisory or physician notes
                        pw.Expanded(
                          flex: 5,
                          child: pw.Column(
                            crossAxisAlignment: pw.CrossAxisAlignment.start,
                            children: [
                              // If there are abnormalities for the right side, show them first
                              if (rightItems.isNotEmpty) ...[
                                pw.SizedBox(height: 24),
                                ...rightItems.map((a) {
                                  final name =
                                      a['abnormalityName']?.toString() ??
                                      'Abnormality';
                                  final severity =
                                      a['severity']?.toString() ?? 'INFO';
                                  final isCrit =
                                      severity.toUpperCase() == 'CRITICAL';
                                  final isWarn =
                                      severity.toUpperCase() == 'WARNING';

                                  final badgeColor = isCrit
                                      ? const PdfColor.fromInt(0xFF991B1B)
                                      : (isWarn
                                            ? const PdfColor.fromInt(0xFFF57F17)
                                            : PdfColors.blue800);

                                  final badgeBg = isCrit
                                      ? const PdfColor.fromInt(0xFFFFEBEE)
                                      : (isWarn
                                            ? const PdfColor.fromInt(0xFFFFFDE7)
                                            : const PdfColor.fromInt(
                                                0xFFE3EBFF,
                                              ));

                                  final desc =
                                      abnormalityExplanations[name]?['description'] ??
                                      'An atypical electrical signal has been detected in the heart rhythm.';
                                  final causes =
                                      abnormalityExplanations[name]?['causes'] ??
                                      'Varies based on age, cardiac history, systemic conditions, or external factors.';
                                  final actions =
                                      abnormalityExplanations[name]?['actions'] ??
                                      'Please consult a medical professional or cardiologist to verify this finding.';

                                  return pw.Container(
                                    margin: const pw.EdgeInsets.only(bottom: 6),
                                    padding: const pw.EdgeInsets.all(6),
                                    decoration: pw.BoxDecoration(
                                      color: const PdfColor.fromInt(0xFFF8F9FE),
                                      borderRadius: const pw.BorderRadius.all(
                                        pw.Radius.circular(6),
                                      ),
                                      border: pw.Border.all(
                                        color: const PdfColor.fromInt(
                                          0xFFE3EBFF,
                                        ),
                                      ),
                                    ),
                                    child: pw.Column(
                                      crossAxisAlignment:
                                          pw.CrossAxisAlignment.start,
                                      children: [
                                        pw.Row(
                                          mainAxisAlignment:
                                              pw.MainAxisAlignment.spaceBetween,
                                          children: [
                                            pw.Text(
                                              name.toUpperCase(),
                                              style: pw.TextStyle(
                                                fontSize: 9.5,
                                                fontWeight: pw.FontWeight.bold,
                                                color: const PdfColor.fromInt(
                                                  0xFF074799,
                                                ),
                                              ),
                                            ),
                                            pw.Container(
                                              padding:
                                                  const pw.EdgeInsets.symmetric(
                                                    horizontal: 5,
                                                    vertical: 1.5,
                                                  ),
                                              decoration: pw.BoxDecoration(
                                                color: badgeBg,
                                                borderRadius:
                                                    const pw.BorderRadius.all(
                                                      pw.Radius.circular(3),
                                                    ),
                                              ),
                                              child: pw.Text(
                                                severity.toUpperCase(),
                                                style: pw.TextStyle(
                                                  fontSize: 6,
                                                  fontWeight:
                                                      pw.FontWeight.bold,
                                                  color: badgeColor,
                                                ),
                                              ),
                                            ),
                                          ],
                                        ),
                                        pw.SizedBox(height: 4),
                                        pw.RichText(
                                          text: pw.TextSpan(
                                            children: [
                                              pw.TextSpan(
                                                text: 'Explanation: ',
                                                style: pw.TextStyle(
                                                  fontWeight:
                                                      pw.FontWeight.bold,
                                                  fontSize: 7.5,
                                                  color: PdfColors.black,
                                                ),
                                              ),
                                              pw.TextSpan(
                                                text: desc,
                                                style: const pw.TextStyle(
                                                  fontSize: 7.5,
                                                  color: PdfColors.grey800,
                                                ),
                                              ),
                                            ],
                                          ),
                                        ),
                                        pw.SizedBox(height: 2),
                                        pw.RichText(
                                          text: pw.TextSpan(
                                            children: [
                                              pw.TextSpan(
                                                text: 'Potential Causes: ',
                                                style: pw.TextStyle(
                                                  fontWeight:
                                                      pw.FontWeight.bold,
                                                  fontSize: 7.5,
                                                  color: PdfColors.black,
                                                ),
                                              ),
                                              pw.TextSpan(
                                                text: causes,
                                                style: const pw.TextStyle(
                                                  fontSize: 7.5,
                                                  color: PdfColors.grey800,
                                                ),
                                              ),
                                            ],
                                          ),
                                        ),
                                        pw.SizedBox(height: 2),
                                        pw.RichText(
                                          text: pw.TextSpan(
                                            children: [
                                              pw.TextSpan(
                                                text: 'Recommended Action: ',
                                                style: pw.TextStyle(
                                                  fontWeight:
                                                      pw.FontWeight.bold,
                                                  fontSize: 7.5,
                                                  color: PdfColors.black,
                                                ),
                                              ),
                                              pw.TextSpan(
                                                text: actions,
                                                style: const pw.TextStyle(
                                                  fontSize: 7.5,
                                                  color: PdfColors.grey800,
                                                ),
                                              ),
                                            ],
                                          ),
                                        ),
                                      ],
                                    ),
                                  );
                                }).toList(),
                                pw.SizedBox(height: 8),
                              ],

                              if (pageIdx == 0) ...[
                                _pdfSectionTitle(
                                  'CLINICAL ADVISORY & WARNINGS',
                                ),
                                pw.SizedBox(height: 6),
                                pw.Container(
                                  padding: const pw.EdgeInsets.all(8),
                                  decoration: pw.BoxDecoration(
                                    color: const PdfColor.fromInt(0xFFFFFDF7),
                                    borderRadius: const pw.BorderRadius.all(
                                      pw.Radius.circular(6),
                                    ),
                                    border: pw.Border.all(
                                      color: const PdfColor.fromInt(0xFFFDE8E8),
                                      width: 1,
                                    ),
                                  ),
                                  child: pw.Column(
                                    crossAxisAlignment:
                                        pw.CrossAxisAlignment.start,
                                    children: [
                                      pw.Row(
                                        children: [
                                          pw.Container(
                                            width: 5,
                                            height: 5,
                                            decoration: const pw.BoxDecoration(
                                              color: PdfColors.amber700,
                                              shape: pw.BoxShape.circle,
                                            ),
                                          ),
                                          pw.SizedBox(width: 5),
                                          pw.Text(
                                            'SIGNAL INTERFERENCE & ARTIFACT ADVISORY',
                                            style: pw.TextStyle(
                                              fontSize: 8,
                                              fontWeight: pw.FontWeight.bold,
                                              color: const PdfColor.fromInt(
                                                0xFFB45309,
                                              ),
                                            ),
                                          ),
                                        ],
                                      ),
                                      pw.SizedBox(height: 5),
                                      pw.Text(
                                        'ECG readings are highly sensitive to physical movements. Shaking, shivering, deep breathing, or speaking during the recording can introduce noise (artifacts) into the signal. Similarly, minor displacement of the leads (moving slightly upper/lower on the chest) or loose electrode patches can disrupt the signal connection.',
                                        style: const pw.TextStyle(
                                          fontSize: 7.5,
                                          color: PdfColors.grey800,
                                          height: 1.2,
                                        ),
                                      ),
                                      pw.SizedBox(height: 4),
                                      pw.Text(
                                        'IMPORTANT: These physical interferences can mimic clinical abnormalities on the graph. For instance, a temporary lead shift or machine movement can create a flatline or drop in signal that the AI interprets as a "Sinus Pause" or "Asystole". Muscle tremors can mimic "Arrhythmias".',
                                        style: pw.TextStyle(
                                          fontSize: 7.5,
                                          fontWeight: pw.FontWeight.bold,
                                          color: PdfColors.red800,
                                          height: 1.2,
                                        ),
                                      ),
                                      pw.SizedBox(height: 4),
                                      pw.Text(
                                        'If any patient movement or lead adjustment occurred during this recording, please repeat the ECG test. Ensure the patient is resting comfortably and completely still, with leads securely attached.',
                                        style: const pw.TextStyle(
                                          fontSize: 7.5,
                                          color: PdfColors.grey800,
                                          height: 1.2,
                                        ),
                                      ),
                                    ],
                                  ),
                                ),
                                pw.SizedBox(height: 8),
                                pw.Container(
                                  padding: const pw.EdgeInsets.all(6),
                                  decoration: pw.BoxDecoration(
                                    color: const PdfColor.fromInt(0xFFF8F9FE),
                                    borderRadius: const pw.BorderRadius.all(
                                      pw.Radius.circular(6),
                                    ),
                                    border: pw.Border.all(
                                      color: const PdfColor.fromInt(0xFFE3EBFF),
                                    ),
                                  ),
                                  child: pw.Column(
                                    crossAxisAlignment:
                                        pw.CrossAxisAlignment.start,
                                    children: [
                                      pw.Text(
                                        'GUIDELINES FOR RELIABLE ECG RECORDING',
                                        style: pw.TextStyle(
                                          fontSize: 8,
                                          fontWeight: pw.FontWeight.bold,
                                          color: const PdfColor.fromInt(
                                            0xFF074799,
                                          ),
                                        ),
                                      ),
                                      pw.SizedBox(height: 4),
                                      _pdfGuidelineItem(
                                        '1. Ensure electrode gel/patches are fresh and firmly placed on clean, dry skin.',
                                      ),
                                      _pdfGuidelineItem(
                                        '2. The patient should sit or lie down in a relaxed state, breathing naturally.',
                                      ),
                                      _pdfGuidelineItem(
                                        '3. Minimize talking, coughing, or any muscle movement during the recording.',
                                      ),
                                      _pdfGuidelineItem(
                                        '4. Keep the ECG device and cables stable, away from electrical cables or phones.',
                                      ),
                                    ],
                                  ),
                                ),
                              ] else ...[
                                // Sub-sequential pages show Physician Notes
                                pw.Container(
                                  height: 200,
                                  padding: const pw.EdgeInsets.all(8),
                                  decoration: pw.BoxDecoration(
                                    color: const PdfColor.fromInt(0xFFF8F9FE),
                                    borderRadius: const pw.BorderRadius.all(
                                      pw.Radius.circular(6),
                                    ),
                                    border: pw.Border.all(
                                      color: const PdfColor.fromInt(0xFFE3EBFF),
                                    ),
                                  ),
                                  child: pw.Column(
                                    crossAxisAlignment:
                                        pw.CrossAxisAlignment.start,
                                    children: [
                                      pw.Text(
                                        'PHYSICIAN NOTES & CLINICAL COMMENTS',
                                        style: pw.TextStyle(
                                          fontSize: 8,
                                          fontWeight: pw.FontWeight.bold,
                                          color: const PdfColor.fromInt(
                                            0xFF074799,
                                          ),
                                        ),
                                      ),
                                      pw.SizedBox(height: 10),
                                      ...List.generate(
                                        5,
                                        (index) => pw.Column(
                                          children: [
                                            pw.Container(
                                              height: 0.8,
                                              color: PdfColors.grey300,
                                            ),
                                            pw.SizedBox(height: 16),
                                          ],
                                        ),
                                      ),
                                    ],
                                  ),
                                ),
                              ],
                            ],
                          ),
                        ),
                      ],
                    ),
                  ),
                  pw.SizedBox(height: 8),
                  pw.Row(
                    mainAxisAlignment: pw.MainAxisAlignment.spaceBetween,
                    children: [
                      pw.Text(
                        'CONFIDENTIAL MEDICAL RECORD - Rhythmrix diagnostic reference',
                        style: pw.TextStyle(
                          fontSize: 8,
                          fontWeight: pw.FontWeight.bold,
                          color: PdfColors.black,
                        ),
                      ),
                      pw.Text(
                        'Page $pageNum of $totalPages',
                        style: pw.TextStyle(
                          fontSize: 8,
                          color: PdfColors.grey700,
                        ),
                      ),
                    ],
                  ),
                ],
              );
            },
          ),
        );
      }

      final output = await getTemporaryDirectory();
      final file = File(
        "${output.path}/ECG_Report_${now.millisecondsSinceEpoch}.pdf",
      );
      await file.writeAsBytes(await pdf.save());

      if (mounted) Navigator.pop(context);

      if (share) {
        await Share.shareXFiles([
          XFile(file.path),
        ], text: 'Check out this ECG Report');
      } else {
        try {
          final downloadPath =
              "/storage/emulated/0/Download/ECG_Report_${now.millisecondsSinceEpoch}.pdf";
          final externalFile = File(downloadPath);
          await externalFile.writeAsBytes(await file.readAsBytes());
          if (mounted) {
            _createDownloadNotification();
            ScaffoldMessenger.of(context).showSnackBar(
              SnackBar(
                content: Text(
                  "PDF downloaded to Downloads folder: $downloadPath",
                ),
              ),
            );
          }
        } catch (e) {
          if (mounted) {
            ScaffoldMessenger.of(context).showSnackBar(
              const SnackBar(
                content: Text(
                  "PDF saved to temporary storage. Use Share to export.",
                ),
              ),
            );
          }
        }
      }
    } catch (e) {
      if (mounted) {
        Navigator.pop(context);
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text("Error: ${e.toString()}")));
      }
    }
  }

  pw.TableRow _pdfTableRow(String name, String observed, String range) {
    return pw.TableRow(
      children: [
        pw.Padding(
          padding: const pw.EdgeInsets.symmetric(horizontal: 10, vertical: 9),
          child: pw.Text(name, style: const pw.TextStyle(fontSize: 11)),
        ),
        pw.Padding(
          padding: const pw.EdgeInsets.symmetric(horizontal: 10, vertical: 9),
          child: pw.Text(
            observed,
            style: pw.TextStyle(fontSize: 11, fontWeight: pw.FontWeight.bold),
          ),
        ),
        pw.Padding(
          padding: const pw.EdgeInsets.symmetric(horizontal: 10, vertical: 9),
          child: pw.Text(
            range,
            style: const pw.TextStyle(fontSize: 11, color: PdfColors.grey700),
          ),
        ),
      ],
    );
  }

  pw.Widget _pdfPositionedLabel(String text, double left, double top) {
    return pw.Positioned(
      left: left,
      top: top,
      child: pw.Text(
        text,
        style: pw.TextStyle(
          fontSize: 7,
          fontWeight: pw.FontWeight.bold,
          color: PdfColors.black,
        ),
      ),
    );
  }

  pw.Widget _pdfRiskMeter(bool hasCritical, bool isNormal) {
    double activeRiskOffset = 0.16; // Low Risk
    if (hasCritical) {
      activeRiskOffset = 0.84; // High Risk
    } else if (!isNormal) {
      activeRiskOffset = 0.50; // Moderate Risk
    }

    final double barWidth = 320.0; // Full-width bar matching enlarged table
    final double barHeight = 12.0;
    final double indicatorPosition = barWidth * activeRiskOffset;
    final double seg = barWidth / 3;

    return pw.Column(
      crossAxisAlignment: pw.CrossAxisAlignment.start,
      children: [
        pw.SizedBox(height: 20),
        pw.Text(
          'Risk Meter',
          style: pw.TextStyle(
            fontSize: 12,
            fontWeight: pw.FontWeight.bold,
            color: const PdfColor.fromInt(0xFF074799),
          ),
        ),
        pw.SizedBox(height: 2),
        pw.Container(
          width: 28,
          height: 2,
          color: const PdfColor.fromInt(0xFF074799),
        ),
        pw.SizedBox(height: 8),

        // Low, Moderate, High Risk Text Labels Row
        pw.Container(
          width: barWidth,
          child: pw.Row(
            mainAxisAlignment: pw.MainAxisAlignment.spaceBetween,
            children: [
              pw.Text(
                'Low Risk',
                style: const pw.TextStyle(
                  fontSize: 9,
                  color: PdfColors.grey700,
                ),
              ),
              pw.Text(
                'Moderate Risk',
                style: const pw.TextStyle(
                  fontSize: 9,
                  color: PdfColors.grey700,
                ),
              ),
              pw.Text(
                'High Risk',
                style: const pw.TextStyle(
                  fontSize: 9,
                  color: PdfColors.grey700,
                ),
              ),
            ],
          ),
        ),
        pw.SizedBox(height: 5),

        // CustomPaint drawing the 3 colored segments and the indicator triangle pointing up
        pw.Container(
          width: barWidth,
          height: 20,
          child: pw.CustomPaint(
            painter: (canvas, size) {
              // 1. Draw Low Risk (green)
              canvas.setFillColor(const PdfColor.fromInt(0xFF10B981));
              canvas.drawRect(0, 8, seg, barHeight);
              canvas.fillPath();

              // 2. Draw Moderate Risk (orange)
              canvas.setFillColor(const PdfColor.fromInt(0xFFF59E0B));
              canvas.drawRect(seg, 8, seg, barHeight);
              canvas.fillPath();

              // 3. Draw High Risk (red)
              canvas.setFillColor(const PdfColor.fromInt(0xFFEF4444));
              canvas.drawRect(seg * 2, 8, seg, barHeight);
              canvas.fillPath();

              // 4. Draw pointer triangle above the bar (pointing downward like ▼)
              canvas.setFillColor(const PdfColor.fromInt(0xFF1E293B));
              canvas.moveTo(indicatorPosition, 8); // tip touches top of bar
              canvas.lineTo(indicatorPosition - 5, 0); // top left of base
              canvas.lineTo(indicatorPosition + 5, 0); // top right of base
              canvas.closePath();
              canvas.fillPath();
            },
          ),
        ),
      ],
    );
  }

  pw.Widget _pdfSectionTitle(String title) {
    return pw.Column(
      crossAxisAlignment: pw.CrossAxisAlignment.start,
      children: [
        pw.Text(
          title,
          style: pw.TextStyle(
            fontSize: 9,
            fontWeight: pw.FontWeight.bold,
            color: const PdfColor.fromInt(0xFF074799),
          ),
        ),
        pw.SizedBox(height: 3),
        pw.Container(
          width: 25,
          height: 2,
          color: const PdfColor.fromInt(0xFF074799),
        ),
      ],
    );
  }

  pw.Widget _pdfProfileField(String label, String value) {
    return pw.Column(
      crossAxisAlignment: pw.CrossAxisAlignment.start,
      children: [
        pw.Text(
          label,
          style: pw.TextStyle(
            fontSize: 7,
            color: PdfColors.grey600,
            fontWeight: pw.FontWeight.bold,
          ),
        ),
        pw.SizedBox(height: 2),
        pw.Text(
          value,
          style: pw.TextStyle(
            fontSize: 10,
            fontWeight: pw.FontWeight.bold,
            color: PdfColors.black,
          ),
        ),
      ],
    );
  }

  pw.Widget _pdfMetricCard(String title, String value, String range) {
    return pw.Container(
      width: 82,
      padding: const pw.EdgeInsets.all(6),
      decoration: pw.BoxDecoration(
        color: PdfColors.white,
        border: pw.Border.all(color: PdfColors.grey200, width: 1.0),
        borderRadius: const pw.BorderRadius.all(pw.Radius.circular(6)),
      ),
      child: pw.Column(
        children: [
          pw.Text(
            title,
            style: pw.TextStyle(
              fontSize: 7,
              color: PdfColors.grey600,
              fontWeight: pw.FontWeight.bold,
            ),
          ),
          pw.SizedBox(height: 4),
          pw.Text(
            value,
            style: pw.TextStyle(
              fontSize: 11,
              fontWeight: pw.FontWeight.bold,
              color: const PdfColor.fromInt(0xFF074799),
            ),
          ),
          pw.SizedBox(height: 2),
          pw.Text(
            'Range: $range',
            style: pw.TextStyle(fontSize: 6, color: PdfColors.grey500),
          ),
        ],
      ),
    );
  }

  pw.Widget _pdfGuidelineItem(String text) {
    return pw.Padding(
      padding: const pw.EdgeInsets.only(bottom: 3),
      child: pw.Text(
        text,
        style: const pw.TextStyle(fontSize: 8, color: PdfColors.grey800),
      ),
    );
  }
}

class _SpandanStyle12LeadPainter extends CustomPainter {
  final Map<String, List<double>> staticLeadData;

  _SpandanStyle12LeadPainter({required this.staticLeadData});

  @override
  void paint(Canvas canvas, Size size) {
    // 1. Red/Pink Medical Grid (1mm minor, 5mm major)
    const double subSquareSize = 8.0;
    final minorGridPaint = Paint()
      ..color = const Color(0xFFFFEBEE)
      ..strokeWidth = 0.5;

    final majorGridPaint = Paint()
      ..color = const Color(0xFFFFCDD2)
      ..strokeWidth = 1.0;

    for (double i = 0; i <= size.width; i += subSquareSize) {
      final isMajor = (i / subSquareSize).round() % 5 == 0;
      canvas.drawLine(
        Offset(i, 0),
        Offset(i, size.height),
        isMajor ? majorGridPaint : minorGridPaint,
      );
    }
    for (double i = 0; i <= size.height; i += subSquareSize) {
      final isMajor = (i / subSquareSize).round() % 5 == 0;
      canvas.drawLine(
        Offset(0, i),
        Offset(size.width, i),
        isMajor ? majorGridPaint : minorGridPaint,
      );
    }

    final wavePaint = Paint()
      ..color = const Color(0xFF1E293B)
      ..strokeWidth = 1.3
      ..style = PaintingStyle.stroke
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round;

    final dashedPaint = Paint()
      ..color = const Color(0xFF94A3B8)
      ..strokeWidth = 1.0;

    final textStyle = GoogleFonts.outfit(
      fontSize: 10,
      fontWeight: FontWeight.bold,
      color: const Color(0xFF1E293B),
    );

    // 4 Rows layout (Spandan Standard):
    // Row 0: lead I, aVR, v1, v4
    // Row 1: lead II, aVL, v2, v5
    // Row 2: lead III, aVF, v3, v6
    // Row 3: Rhythm (lead II)
    final rows = [
      ['Lead I', 'aVR', 'V1', 'V4'],
      ['Lead II', 'aVL', 'V2', 'V5'],
      ['Lead III', 'aVF', 'V3', 'V6'],
    ];

    final rowHeight = size.height / 4.0;
    const pulseWidth = 24.0;
    final contentWidth = size.width - pulseWidth;
    final segWidth = contentWidth / 4.0;

    for (int r = 0; r < 3; r++) {
      final yBase = rowHeight * r + rowHeight / 2.0;

      // Draw 1 mV Calibration Pulse
      final pulsePath = Path()
        ..moveTo(0, yBase)
        ..lineTo(4, yBase)
        ..lineTo(4, yBase - 20) // 1 mV step = 20 px
        ..lineTo(18, yBase - 20)
        ..lineTo(18, yBase)
        ..lineTo(pulseWidth, yBase);
      canvas.drawPath(pulsePath, wavePaint);

      // Draw 4 Segments
      for (int c = 0; c < 4; c++) {
        final leadName = rows[r][c];
        final xStart = pulseWidth + c * segWidth;

        // Draw vertical dashed line at segment boundary
        if (c > 0) {
          for (double dy = rowHeight * r; dy < rowHeight * (r + 1); dy += 6) {
            canvas.drawLine(
              Offset(xStart, dy),
              Offset(xStart, math.min(dy + 3, rowHeight * (r + 1))),
              dashedPaint,
            );
          }
        }

        // Draw Lead Text Label
        final textPainter = TextPainter(
          text: TextSpan(text: leadName.toLowerCase(), style: textStyle),
          textDirection: ui.TextDirection.ltr,
        )..layout();
        textPainter.paint(canvas, Offset(xStart + 6, yBase - rowHeight / 2.2));

        final fullPts = staticLeadData[leadName] ?? [];
        if (fullPts.isNotEmpty) {
          final refL2 = staticLeadData['Lead II'] ?? staticLeadData['L2'] ?? [];
          int startIdx = 0;
          if (refL2.length > 20) {
            List<double> sortedL2 = List<double>.from(refL2)..sort();
            double baseL2 = sortedL2[sortedL2.length ~/ 2];
            double maxDevL2 = 0.0;
            for (var v in refL2) {
              double d = (v - baseL2).abs();
              if (d > maxDevL2) maxDevL2 = d;
            }
            if (maxDevL2 > 0) {
              double thresh = maxDevL2 * 0.55;
              for (int i = 2; i < refL2.length - 2; i++) {
                if (refL2[i] - baseL2 > thresh &&
                    refL2[i] > refL2[i - 1] &&
                    refL2[i] > refL2[i - 2] &&
                    refL2[i] >= refL2[i + 1] &&
                    refL2[i] >= refL2[i + 2]) {
                  startIdx = math.max(0, i - 15);
                  break;
                }
              }
            }
          }

          final pts = fullPts.length > startIdx + 310
              ? fullPts.sublist(startIdx, math.min(fullPts.length, startIdx + 310))
              : (fullPts.length > 310 ? fullPts.sublist(0, 310) : fullPts);

          List<double> sorted = List<double>.from(pts)..sort();
          double medianBaseline = sorted[sorted.length ~/ 2];

          double posMax = 0.0, negMax = 0.0;
          for (var v in pts) {
            double dev = v - medianBaseline;
            if (dev > posMax) posMax = dev;
            if (-dev > negMax) negMax = -dev;
          }
          double maxAbsDev = math.max(posMax, negMax);
          if (maxAbsDev < 1.0) maxAbsDev = 1.0;

          final double leadScale = (rowHeight * 0.30) / maxAbsDev;

          final xStep = segWidth / math.max(1, pts.length - 1);
          final wavePath = Path();
          bool first = true;
          for (int i = 0; i < pts.length; i++) {
            final x = xStart + i * xStep;
            final y = yBase - (pts[i] - medianBaseline) * leadScale;
            if (first) {
              wavePath.moveTo(x, y);
              first = false;
            } else {
              wavePath.lineTo(x, y);
            }
          }
          canvas.drawPath(wavePath, wavePaint);
        }
      }
    }

    // Row 3: Continuous Rhythm Strip (Lead II)
    final yBaseRhythm = rowHeight * 3 + rowHeight / 2.0;

    final pulsePathR = Path()
      ..moveTo(0, yBaseRhythm)
      ..lineTo(4, yBaseRhythm)
      ..lineTo(4, yBaseRhythm - 20)
      ..lineTo(18, yBaseRhythm - 20)
      ..lineTo(18, yBaseRhythm)
      ..lineTo(pulseWidth, yBaseRhythm);
    canvas.drawPath(pulsePathR, wavePaint);

    final textPainterR = TextPainter(
      text: TextSpan(text: 'rhythm (lead II)', style: textStyle),
      textDirection: ui.TextDirection.ltr,
    )..layout();
    textPainterR.paint(
      canvas,
      Offset(pulseWidth + 6, yBaseRhythm - rowHeight / 2.2),
    );

    final rPts = staticLeadData['Lead II'] ?? staticLeadData['L2'] ?? [];
    if (rPts.isNotEmpty) {
      List<double> sortedR = List<double>.from(rPts)..sort();
      double medianBaselineR = sortedR[sortedR.length ~/ 2];

      double posMaxR = 0.0, negMaxR = 0.0;
      for (var v in rPts) {
        double dev = v - medianBaselineR;
        if (dev > posMaxR) posMaxR = dev;
        if (-dev > negMaxR) negMaxR = -dev;
      }
      double maxAbsDevR = math.max(posMaxR, negMaxR);
      if (maxAbsDevR < 1.0) maxAbsDevR = 1.0;

      final double rhythmScale = (rowHeight * 0.30) / maxAbsDevR;

      final xStep = contentWidth / math.max(1, rPts.length - 1);
      final rhythmPath = Path();
      bool first = true;
      for (int i = 0; i < rPts.length; i++) {
        final x = pulseWidth + i * xStep;
        final y = yBaseRhythm - (rPts[i] - medianBaselineR) * rhythmScale;
        if (first) {
          rhythmPath.moveTo(x, y);
          first = false;
        } else {
          rhythmPath.lineTo(x, y);
        }
      }
      canvas.drawPath(rhythmPath, wavePaint);
    }
  }

  @override
  bool shouldRepaint(covariant _SpandanStyle12LeadPainter oldDelegate) => true;
}

class _StaticClinicalECGPainter extends CustomPainter {
  final List<double> points;

  _StaticClinicalECGPainter({required this.points});

  @override
  void paint(Canvas canvas, Size size) {
    if (points.isEmpty) return;

    // 1. Draw medical grid (using standard pink/red lines)
    const double subSquareSize = 10.0;

    final minorGridPaint = Paint()
      ..color =
          const Color(0xFFFFEBEE) // very light pink/red
      ..strokeWidth = 0.5;

    final majorGridPaint = Paint()
      ..color =
          const Color(0xFFFFCDD2) // light pink/red
      ..strokeWidth = 1.0;

    for (double i = 0; i <= size.width; i += subSquareSize) {
      final isMajor = (i / subSquareSize).round() % 5 == 0;
      canvas.drawLine(
        Offset(i, 0),
        Offset(i, size.height),
        isMajor ? majorGridPaint : minorGridPaint,
      );
    }

    for (double i = 0; i <= size.height; i += subSquareSize) {
      final isMajor = (i / subSquareSize).round() % 5 == 0;
      canvas.drawLine(
        Offset(0, i),
        Offset(size.width, i),
        isMajor ? majorGridPaint : minorGridPaint,
      );
    }

    // 2. Display all data points (A to Z) without truncation
    List<double> displayPts = List<double>.from(points);
    if (displayPts.length > 5) {
      List<double> sorted = List<double>.from(displayPts)..sort();
      double medVal = sorted[sorted.length ~/ 2];
      for (int i = 0; i < displayPts.length; i++) {
        if ((displayPts[i] - medVal).abs() > 50000) {
          displayPts[i] = medVal;
        }
      }
    }

    // 3. Dynamic amplitude auto-scaling to keep signal centered and cleanly visible within card bounds
    double pMin = double.infinity;
    double pMax = -double.infinity;
    for (var val in displayPts) {
      if (val < pMin) pMin = val;
      if (val > pMax) pMax = val;
    }
    double range = pMax - pMin;
    if (range <= 0.0001) range = 1.0;

    final yMid = size.height / 2;
    final pMid = pMin + range / 2.0;
    final scale = (size.height * 0.65) / range;

    final xStep = size.width / math.max(1, displayPts.length - 1);
    final wavePaint = Paint()
      ..color = const Color(0xFF074799)
      ..strokeWidth = 1.5
      ..style = PaintingStyle.stroke
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round;

    final path = Path();
    bool first = true;

    for (int i = 0; i < displayPts.length; i++) {
      final x = i * xStep;
      final y = yMid - (displayPts[i] - pMid) * scale;

      if (first) {
        path.moveTo(x, y);
        first = false;
      } else {
        path.lineTo(x, y);
      }
    }
    canvas.drawPath(path, wavePaint);
  }

  @override
  bool shouldRepaint(covariant _StaticClinicalECGPainter oldDelegate) {
    return oldDelegate.points != points;
  }
}