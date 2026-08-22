/// App theme (Docs/App/General info.md: grey-ish blue, orange, white).
library;

import 'package:flutter/material.dart';

const Color kSurface = Color(0xFF353C3F); // grey-blue background
const Color kSurfaceAlt = Color(0xFF4F575C); // bubble grey
const Color kOrange = Color(0xFFF57600); // accent orange
const Color kOrangeDark = Color(0xFFE16D00);
const Color kWhite = Colors.white;

ThemeData buildTheme() {
  return ThemeData(
    useMaterial3: true,
    brightness: Brightness.dark,
    colorScheme: const ColorScheme(
      brightness: Brightness.dark,
      primary: kOrange,
      onPrimary: Colors.black,
      secondary: kSurfaceAlt,
      onSecondary: kWhite,
      surface: kSurface,
      onSurface: kWhite,
      error: Colors.red,
      onError: kWhite,
    ),
    scaffoldBackgroundColor: kSurface,
    appBarTheme: const AppBarTheme(
      backgroundColor: kOrangeDark,
      foregroundColor: Colors.black,
      elevation: 0,
      scrolledUnderElevation: 0,
      surfaceTintColor: Colors.transparent,
      centerTitle: false,
      iconTheme: IconThemeData(color: Colors.black),
      titleTextStyle: TextStyle(
          color: Colors.black, fontWeight: FontWeight.w600, fontSize: 20),
    ),
    filledButtonTheme: FilledButtonThemeData(
      style: FilledButton.styleFrom(
        backgroundColor: kSurfaceAlt,
        foregroundColor: kWhite,
        shape:
            RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
        elevation: 0,
      ),
    ),
    dividerTheme: const DividerThemeData(color: kOrange, thickness: 1, space: 1),
    textTheme: const TextTheme(
      bodyLarge: TextStyle(color: kWhite, fontSize: 16),
      bodyMedium: TextStyle(color: kWhite, fontSize: 15),
      bodySmall: TextStyle(color: Colors.white70, fontSize: 12),
    ),
    snackBarTheme: const SnackBarThemeData(
      backgroundColor: kSurfaceAlt,
      contentTextStyle: TextStyle(color: kWhite, fontSize: 14),
      behavior: SnackBarBehavior.floating,
      shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.all(Radius.circular(8))),
    ),
    navigationRailTheme: const NavigationRailThemeData(
      backgroundColor: kSurfaceAlt,
      indicatorColor: kOrangeDark,
      selectedIconTheme: IconThemeData(color: Colors.black),
      unselectedIconTheme: IconThemeData(color: Colors.white70),
      selectedLabelTextStyle: TextStyle(color: kOrange, fontSize: 12),
      unselectedLabelTextStyle: TextStyle(color: Colors.white70, fontSize: 12),
    ),
  );
}
