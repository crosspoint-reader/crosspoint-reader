import React, { useState } from 'react';
import { View, Text, TouchableOpacity, StyleSheet } from 'react-native';
import { useNavigation } from '@react-navigation/native';

const BookMenuScreen = () => {
  const navigation = useNavigation();
  const [selectedOption, setSelectedOption] = useState<string | null>(null);

  const handleOptionPress = (option: string) => {
    setSelectedOption(option);
    // Navigate based on option
    switch (option) {
      case 'Go to %':
        navigation.navigate('BookReader', { screen: 'GoToPage' });
        break;
      case 'Select Chapter':
        navigation.navigate('BookReader', { screen: 'SelectChapter' });
        break;
      case 'Show Page as QR':
        navigation.navigate('BookReader', { screen: 'QRCodePage' });
        break;
      default:
        break;
    }
  };

  const handleBackPress = () => {
    // Always return to the previous screen (menu)
    navigation.goBack();
  };

  return (
    <View style={styles.container}>
      <Text style={styles.title}>Book Menu</Text>
      <TouchableOpacity style={styles.button} onPress={() => handleOptionPress('Go to %')}>
        <Text>Go to %</Text>
      </TouchableOpacity>
      <TouchableOpacity style={styles.button} onPress={() => handleOptionPress('Select Chapter')}>
        <Text>Select Chapter</Text>
      </TouchableOpacity>
      <TouchableOpacity style={styles.button} onPress={() => handleOptionPress('Show Page as QR')}>
        <Text>Show Page as QR</Text>
      </TouchableOpacity>
      <TouchableOpacity style={styles.backButton} onPress={handleBackPress}>
        <Text>Back</Text>
      </TouchableOpacity>
    </View>
  );
};

const styles = StyleSheet.create({
  container: {
    flex: 1,
    padding: 20,
    backgroundColor: '#fff',
  },
  title: {
    fontSize: 24,
    marginBottom: 30,
  },
  button: {
    backgroundColor: '#007AFF',
    padding: 15,
    borderRadius: 8,
    marginBottom: 10,
    alignItems: 'center',
  },
  backButton: {
    position: 'absolute',
    top: 20,
    left: 20,
    backgroundColor: '#666',
    padding: 10,
    borderRadius: 8,
    alignItems: 'center',
  },
});

export default BookMenuScreen;