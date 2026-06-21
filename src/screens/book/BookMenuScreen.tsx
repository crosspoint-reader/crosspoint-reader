import React, { useState } from 'react';
import { View, Text, TouchableOpacity, StyleSheet } from 'react-native';
import { useNavigation } from '@react-navigation/native';

const BookMenuScreen: React.FC = () => {
  const navigation = useNavigation();
  const [selectedOption, setSelectedOption] = useState<string | null>(null);

  const handleOptionPress = (option: string) => {
    setSelectedOption(option);
    navigation.navigate('BookDetail', { option });
  };

  return (
    <View style={styles.container}>
      <TouchableOpacity style={styles.backButton} onPress={() => navigation.goBack()}>
        <Text style={styles.backButtonText}>Back</Text>
      </TouchableOpacity>

      <View style={styles.options}>
        <TouchableOpacity style={styles.option} onPress={() => handleOptionPress('Go to %')}>
          <Text style={styles.optionText}>Go to %</Text>
        </TouchableOpacity>
        <TouchableOpacity style={styles.option} onPress={() => handleOptionPress('Select Chapter')}>
          <Text style={styles.optionText}>Select Chapter</Text>
        </TouchableOpacity>
        <TouchableOpacity style={styles.option} onPress={() => handleOptionPress('Show Page as QR')}>
          <Text style={styles.optionText}>Show Page as QR</Text>
        </TouchableOpacity>
      </View>
    </View>
  );
};

const styles = StyleSheet.create({
  container: {
    flex: 1,
    padding: 16,
    backgroundColor: '#fff',
  },
  backButton: {
    paddingVertical: 8,
    paddingHorizontal: 12,
    backgroundColor: '#007AFF',
    borderRadius: 8,
    alignItems: 'center',
    marginBottom: 16,
  },
  backButtonText: {
    color: '#fff',
    fontSize: 16,
  },
  options: {
    flex: 1,
  },
  option: {
    padding: 12,
    backgroundColor: '#f0f0f0',
    borderRadius: 8,
    marginBottom: 8,
  },
  optionText: {
    fontSize: 16,
    fontWeight: '600',
  },
});

export default BookMenuScreen;